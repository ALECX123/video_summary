#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace bili {

struct VideoInfo {
    std::string title;      // UTF-8, as returned by the API
    std::string bvid;       // empty when the link is av-only
    long long aid = 0;
    long long cid = 0;
    int durationSec = 0;
};

// stage = Chinese message ("解析链接", "下载视频流 12.3/80.0 MB", "合并音视频"...).
// fraction = 0.0..1.0, or -1 when unknown. Called from the calling thread only.
using ProgressFn = std::function<void(const std::string& stage, double fraction)>;

// True for http(s) bilibili video links (bilibili.com/video/BV..|av.., b23.tv/.., bare BV..id).
bool looksLikeBiliUrl(const std::string& input);

// Blocking: resolve URL (follow b23.tv redirects, view API) -> info. err = UTF-8 Chinese message.
bool resolve(const std::string& url, VideoInfo& info, std::string& err,
             const ProgressFn& progress, std::atomic<bool>* cancel);

// Blocking full pipeline: resolve + playurl (DASH) + stream download via httplib + ffmpeg merge.
// Creates outDir if needed. Produces outDir/<sanitized title>.mp4; temp *.Video.*/*.Audio.*
// intermediates removed on success. outFileOut = UTF-8 path of the mp4.
// cancel may be nullptr; when non-null it is polled during stream download.
bool download(const std::string& url, const std::filesystem::path& outDir,
              const std::filesystem::path& ffmpegExe, VideoInfo& info,
              std::filesystem::path& outFileOut, std::string& err,
              const ProgressFn& progress, std::atomic<bool>* cancel);

// ---------------- Cookie / login (process-wide, thread-safe) ----------------
// Cookie string form: "SESSDATA=..; bili_jct=..; DedeUserID=..". Empty = anonymous.
// Persisted by the GUI (QSettings); the module itself keeps it in memory only.
void setCookie(const std::string& cookie);
std::string getCookie();

// QR login: GET passport.bilibili.com/x/passport-login/web/qrcode/generate
// -> qrUrl (encode THIS into the QR image), qrKey for polling.
bool qrStart(std::string& qrUrl, std::string& qrKey, std::string& err);

enum class QrPoll {
    Waiting,    // code 86101: not scanned yet
    Scanned,    // code 86090: scanned, waiting confirm
    Success,    // code 0: Set-Cookie captured into the module cookie store
    Expired,    // code 86038: expired (caller should qrStart again)
    Error,      // network/JSON failure; err filled with Chinese message
};
// Single non-blocking poll of .../qrcode/poll?qrcode_key=.. (use a ~2s timer in the GUI).
QrPoll qrPoll(const std::string& qrKey, std::string& err);

// GET api.bilibili.com/x/web-interface/nav with the current cookie.
// loggedIn=false with err=="" is NOT an error (anonymous). uname only valid when loggedIn.
bool navLoginState(bool& loggedIn, std::string& uname, std::string& err);

}  // namespace bili
