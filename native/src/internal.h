#ifndef KLIVE_INTERNAL_H
#define KLIVE_INTERNAL_H

#include "klive.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

/**
 * 用 SRWLOCK 而不是 std::mutex：DLL 被 JVM 通过 JNA 加载后，MSVC 的 std::mutex
 * 加锁会直接触发访问违例（PowerShell P/Invoke 调用同一函数却正常）。SRWLOCK
 * 零初始化、只依赖 kernel32，跨运行时更稳。
 */
class SrwExclusiveLock {
public:
    explicit SrwExclusiveLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~SrwExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }

    SrwExclusiveLock(const SrwExclusiveLock&) = delete;
    SrwExclusiveLock& operator=(const SrwExclusiveLock&) = delete;

private:
    SRWLOCK& lock_;
};

/* 枚举顺序即对外的 monitor index，镜像时靠同一顺序定位同一台显示器。 */
struct KliveOutput {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    LUID luid{};
    int32_t index = 0;
    float refresh_hz = 0.0f;
    char name[64]{};
    char friendly_name[96]{};
    char adapter_name[96]{};
};

struct KliveMirrorSession {
    SRWLOCK lifecycle = SRWLOCK_INIT;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    std::atomic<int32_t> last_error{0};
    std::atomic<int64_t> frames{0};
    std::atomic<double> fps{0.0};
    std::atomic<int32_t> frame_width{0};
    std::atomic<int32_t> frame_height{0};
    std::atomic<int32_t> mode{KLIVE_MODE_FULLSCREEN};
    std::atomic<int32_t> source_index{-1};
    klive_mirror_config config{};
};

std::vector<KliveOutput> klive_collect_outputs();

std::vector<klive_window_desc> klive_collect_windows();

/* 窗口是否可以作为采集源：可见、有标题、非工具窗、非本进程、未被 DWM cloak、尺寸有效。 */
bool klive_is_capturable_window(HWND window);

KliveMirrorSession& klive_session();

void klive_set_message(const char* fmt, ...);
const char* klive_message();

int32_t klive_mirror_start(const klive_mirror_config& config);
int32_t klive_mirror_stop();

#endif /* KLIVE_INTERNAL_H */
