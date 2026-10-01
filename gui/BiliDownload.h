#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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

// ---------------- Private messages (私信, message/private_msg.md) ----------------
// All blocking; call from worker threads; err = UTF-8 Chinese message.
// GET endpoints use the cookie (SESSDATA); sendPmText additionally needs
// csrf (bili_jct cookie) and a Wbi signature. sendPmText also marks the
// target session read server-side (see 发送私信 doc note).

struct PmSession {
    long long talkerId = 0;   // peer mid (session_type is always 1 here)
    int unread = 0;
    long long ackSeqno = 0;   // newest seqno already read
    long long maxSeqno = 0;   // newest seqno in the session
    long long lastSender = 0; // last_msg.sender_uid
    int lastMsgType = 0;
};

struct PmMessage {
    long long senderUid = 0;
    long long seqno = 0;
    long long timestamp = 0;  // seconds
    int msgType = 0;
    int msgStatus = 0;        // 0 = normal; skip 1/2 (recalled)
    std::string content;      // raw `content` field (JSON-encoded payload)
};

// GET api.vc.bilibili.com/session_svr/v1/session_svr/get_sessions
//   ?session_type=4&size=50&sort_rule=1&build=0&mobi_app=web
// Returns ONLY user sessions (session_type==1 && system_msg_type==0),
// newest activity first, capped at 50.
bool fetchPmSessions(std::vector<PmSession>& out, std::string& err);

// GET api.vc.bilibili.com/svr_sync/v1/svr_sync/fetch_session_msgs
//   ?talker_id=<uid>&session_type=1&size=<size>&begin_seqno=<begin>
//   &sender_device_id=1&build=0&mobi_app=web
// Returns messages with seqno > beginSeqno, NEWEST FIRST (reverse order);
// size clamped to 1..200.
bool fetchPmMessages(long long talkerId, long long beginSeqno, int size,
                     std::vector<PmMessage>& out, std::string& err);

// POST api.vc.bilibili.com/web_im/v1/web_im/send_msg (Wbi-signed, csrf).
// Sends UTF-8 text (msg_type=1) to user mid; text is byte-truncated to fit
// the 2000-byte limit. Server marks the session read on success.
bool sendPmText(long long receiverUid, const std::string& text, std::string& err);

// POST api.vc.bilibili.com/session_svr/v1/session_svr/update_ack (csrf).
// Marks the peer session read through ackSeqno (0 = up to the newest).
bool markPmRead(long long talkerId, long long ackSeqno, std::string& err);

// Own mid: api.bilibili.com nav data.mid, falling back to the DedeUserID
// cookie. false + err when neither is available (not logged in).
bool myMid(long long& mid, std::string& err);

// Best-effort video link extraction from a received message:
//   msg_type 1 -> first whitespace-delimited token of the inner text that
//                 looksLikeBiliUrl() accepts (http link or bare BV id);
//   msg_type 7 -> content.url when source==5, else "https://www.bilibili.com/video/<bvid>";
//   otherwise  -> "".
std::string pmVideoLink(const PmMessage& m);

}  // namespace bili
