#include "fault_handler.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace livemap::fault {

namespace {

constexpr size_t kAnchorSize = 2;
constexpr size_t kLineCapacity = 192;
constexpr uintptr_t kInvalidHandle = ~uintptr_t{0};
constexpr std::string_view kProbeSection = ".lmprobe";
static_assert(kProbeSection.size() == IMAGE_SIZEOF_SHORT_NAME);

// Set before the handler is registered; the regions publish atomically, so the handler reads them without a lock.
struct State {
    Layout layout;
    std::atomic<bool> written{false};
    std::atomic<uintptr_t> file{0};
    std::atomic<uintptr_t> handler{0};
};

// constinit: the first call may be the handler's, which must not take an init guard.
State& state() {
    static constinit State instance;
    return instance;
}

const char*& current_site() {
    static constinit thread_local const char* site = nullptr;
    return site;
}

uintptr_t this_module() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       std::bit_cast<LPCWSTR>(&this_module), &self);
    return std::bit_cast<uintptr_t>(self);
}

uintptr_t exe_module() {
    return std::bit_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

const IMAGE_NT_HEADERS* headers_of(uintptr_t base) {
    const auto* dos = std::bit_cast<const IMAGE_DOS_HEADER*>(base);
    return std::bit_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
}

Range image_range(uintptr_t base) {
    return {base, base + headers_of(base)->OptionalHeader.SizeOfImage};
}

Range probe_range(uintptr_t base) {
    const IMAGE_NT_HEADERS* nt = headers_of(base);
    const std::span sections(IMAGE_FIRST_SECTION(nt), nt->FileHeader.NumberOfSections);
    for (const IMAGE_SECTION_HEADER& section : sections) {
        if (std::memcmp(section.Name, kProbeSection.data(), kProbeSection.size()) == 0) {
            return {base + section.VirtualAddress, base + section.VirtualAddress + section.Misc.VirtualSize};
        }
    }
    return {};
}

// The whole allocation a hook's private allocator took, which holds the hook's stub and trampoline.
Range block_of(uintptr_t address) {
    MEMORY_BASIC_INFORMATION inside{};
    MEMORY_BASIC_INFORMATION whole{};
    if (VirtualQuery(std::bit_cast<LPCVOID>(address), &inside, sizeof inside) == 0 ||
        VirtualQuery(inside.AllocationBase, &whole, sizeof whole) == 0) {
        return {};
    }
    const auto base = std::bit_cast<uintptr_t>(inside.AllocationBase);
    return {base, base + whole.RegionSize};
}

void write_line(const State& current, uint32_t code, const Verdict& verdict) {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const Stamp stamp{now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds};
    std::array<char, kLineCapacity> line{};
    if (const size_t length = format_line(line.data(), line.size(), stamp, code, verdict); length > 0) {
        DWORD written = 0;
        WriteFile(std::bit_cast<HANDLE>(current.file.load()), line.data(), static_cast<DWORD>(length), &written, nullptr);
    }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    State& current = state();
    const auto code = static_cast<uint32_t>(info->ExceptionRecord->ExceptionCode);
    if (!is_fault_code(code) || current.written.load()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto address = std::bit_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    if (const Verdict verdict = classify(current.layout, address, code, current_site());
        verdict.where != Where::Outside && !current.written.exchange(true)) {
        write_line(current, code, verdict);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool install(uintptr_t file) {
    State& current = state();
    const uintptr_t self = this_module();
    if (file == 0 || file == kInvalidHandle || self == 0 || current.handler.load() != 0) {
        return false;
    }
    SetFilePointerEx(std::bit_cast<HANDLE>(file), LARGE_INTEGER{}, nullptr, FILE_END);
    current.layout.dll = image_range(self);
    current.layout.probes = probe_range(self);
    current.layout.game = image_range(exe_module());
    current.file.store(file);
    // First, so an earlier handler cannot end the process before this one writes; a later first=1 handler, such as the hook library's, still runs ahead.
    const auto handler = std::bit_cast<uintptr_t>(AddVectoredExceptionHandler(1, on_exception));
    current.handler.store(handler);
    return handler != 0;
}

void remove() {
    if (const uintptr_t handler = state().handler.exchange(0); handler != 0) {
        RemoveVectoredExceptionHandler(std::bit_cast<PVOID>(handler));
    }
}

SiteScope::SiteScope(const char* site) : previous_(current_site()) {
    current_site() = site;
}

SiteScope::~SiteScope() {
    current_site() = previous_;
}

Hook::Hook(Hook&& other) noexcept : hook_(std::move(other.hook_)), slot_(std::exchange(other.slot_, kNoSlot)) {}

Hook& Hook::operator=(Hook&& other) noexcept {
    if (this != &other) {
        state().layout.regions.retire(slot_);
        hook_ = std::move(other.hook_);
        slot_ = std::exchange(other.slot_, kNoSlot);
    }
    return *this;
}

Hook::~Hook() {
    state().layout.regions.retire(slot_);
}

std::expected<Hook, safetyhook::MidHook::Error> Hook::create(const char* site, uintptr_t target,
                                                             safetyhook::MidHookFn callback) {
    const auto allocator = safetyhook::Allocator::create();
    // The first allocation of a private allocator opens the block that the hook's stub and trampoline then share.
    auto anchor = allocator->allocate_near({std::bit_cast<uint8_t*>(target)}, kAnchorSize);
    if (!anchor) {
        return std::unexpected(safetyhook::MidHook::Error::bad_allocation(anchor.error()));
    }
    Hook created;
    created.slot_ = state().layout.regions.publish(block_of(std::bit_cast<uintptr_t>(anchor->data())), site);
    auto hook = safetyhook::MidHook::create(allocator, target, callback);
    if (!hook) {
        return std::unexpected(hook.error());
    }
    created.hook_ = std::move(*hook);
    return created;
}

}  // namespace livemap::fault
