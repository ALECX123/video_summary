#include "MessageTab.h"

#include "BiliDownload.h"
#include "Widgets.h"
#include "pipeline.h"
#include "util.h"

#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace {

	constexpr auto kKeyEnable = "message/enabled";
	constexpr auto kKeyInterval = "message/interval";

	// 复用其它标签页已有的设置键 (不另建键)。
	constexpr auto kKeyOutDir = "download/outputDir";
	constexpr auto kKeyModel = "transcribe/model";
	constexpr auto kKeyLanguage = "transcribe/language";
	constexpr auto kKeyFfmpeg = "transcribe/ffmpeg";
	constexpr auto kKeyUseGpu = "transcribe/useGpu";
	constexpr auto kKeyOllamaUrl = "summary/ollamaUrl";
	constexpr auto kKeyOllamaModel = "summary/ollamaModel";

	// 与 SummaryTab 相同的 GUI 默认值 (本机 Ollama 模型, 与 CLI 默认不同)。
	constexpr auto kDefaultOllamaUrl = "http://127.0.0.1:11434";
	constexpr auto kDefaultOllamaModel = "qwen3.5:latest";

	// 文本消息的 content 是 {"text":".."} 形式的 JSON: 提取正文, 压成 60 字单行。
	QString textPreview(const bili::PmMessage& m) {
		std::string text = m.content;
		const std::string key = "\"text\":";
		const size_t pos = text.find(key);
		if (pos != std::string::npos) {
			size_t begin = pos + key.size();
			if (begin < text.size() && text[begin] == '"') {
				++begin;
				size_t end = begin;
				while (end < text.size() && text[end] != '"') {
					if (text[end] == '\\') ++end;  // 转义符跳过其后的字符
					++end;
				}
				text = jsonStringUnescape(text.substr(begin, end - begin));
			}
		}
		QString q = QString::fromUtf8(text).simplified();
		if (q.size() > 60) q = q.left(60) + QStringLiteral("…");
		return q;
	}

	// 发送内容的单行预览: 压平换行并截断 (完整总结较长, 日志只给摘要)。
	QString contentPreview(const QString& text) {
		QString q = text.simplified();
		if (q.size() > 80) q = q.left(80) + QStringLiteral("…");
		return q;
	}

	std::string readFileUtf8(const fs::path& p) {
		std::ifstream ifs(p, std::ios::binary);
		if (!ifs) return {};
		return std::string((std::istreambuf_iterator<char>(ifs)),
			std::istreambuf_iterator<char>());
	}

}  // namespace

MessageTab::MessageTab(QWidget* parent) : QWidget(parent),
ui(new Ui::MessageTab)
{
	ui->setupUi(this);

	// --- 成员别名 (ui 对象名即 QSS 角色) ---
	enableCheck_ = ui->enableCheck;
	intervalSpin_ = ui->intervalSpin;
	stageLabel_ = ui->stageLabel;
	clearBtn_ = ui->btnQuiet;
	log_ = ui->logView;

	// --- 信号 ---
	connect(enableCheck_, &QCheckBox::toggled, this, &MessageTab::onToggleEnabled);
	connect(intervalSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
		&MessageTab::onIntervalChanged);
	connect(clearBtn_, &QPushButton::clicked, this, &MessageTab::onClearLog);
	connect(this, &MessageTab::logReady, this, &MessageTab::onLogLine,
		Qt::QueuedConnection);
	connect(this, &MessageTab::progressReady, this, &MessageTab::onProgress,
		Qt::QueuedConnection);
	connect(this, &MessageTab::verifyFinished, this, &MessageTab::onVerifyFinished,
		Qt::QueuedConnection);

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &MessageTab::pollTick);

	loadSettings();

}

MessageTab::~MessageTab() {
	stopPolling();
	delete ui;
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

void MessageTab::loadSettings() {
	QSettings s;
	const bool enabled = s.value(kKeyEnable, false).toBool();
	const int interval = s.value(kKeyInterval, 60).toInt();

	// 恢复状态但不自动启动: 需要用户重新勾选确认。
	enableCheck_->blockSignals(true);
	intervalSpin_->blockSignals(true);
	enableCheck_->setChecked(enabled);
	intervalSpin_->setValue(interval);
	enableCheck_->blockSignals(false);
	intervalSpin_->blockSignals(false);

	if (enabled) {
		appendLine(QStringLiteral("已恢复设置，勾选\"启用自动处理\"开始轮询"));
		startPolling();
	}
}

void MessageTab::saveSettings() {
	QSettings s;
	s.setValue(kKeyEnable, enableCheck_->isChecked());
	s.setValue(kKeyInterval, intervalSpin_->value());
}

// ---------------------------------------------------------------------------
// 启用 / 停用
// ---------------------------------------------------------------------------

void MessageTab::onToggleEnabled(bool checked) {
	saveSettings();
	if (!checked) {
		stopPolling();
		return;
	}

	// 登录校验走后台线程 (myMid 是阻塞网络请求)。
	stageLabel_->setText(tr("检查登录中…"));
	appendLine(tr("检查登录中…"));
	if (verifyThread_.joinable()) verifyThread_.join();  // 上一次校验已结束
	verifyThread_ = std::thread([this] {
		long long mid = 0;
		std::string err;
		const bool ok = bili::myMid(mid, err);
		emit verifyFinished(ok, QString::fromUtf8(err), static_cast<qint64>(mid));
		});
}

void MessageTab::onVerifyFinished(bool ok, const QString& err, qint64 mid) {
	if (!enableCheck_->isChecked()) return;  // 校验返回前已被取消勾选
	if (!ok) {
		appendLine(err.isEmpty() ? tr("未登录，请先扫码登录")
			: tr("未登录，请先扫码登录: %1").arg(err));
		stageLabel_->setText(tr("就绪"));
		emit statusChanged(tr("就绪"));
		enableCheck_->setChecked(false);  // 触发 onToggleEnabled(false) -> 保存设置
		return;
	}
	if (running_) return;  // 重复到达的校验结果
	appendLine(tr("已登录 (UID %1), 开始轮询私信").arg(mid));
	startPolling();
}

void MessageTab::startPolling() {
	running_ = true;
	stop_ = false;
	cancel_.store(false);

	if (!jobThread_.joinable()) jobThread_ = std::thread(&MessageTab::jobLoop, this);

	timer_->setInterval(intervalSpin_->value() * 1000);
	timer_->start();
	stageLabel_->setText(tr("运行中"));
	emit statusChanged(tr("运行中"));
	pollTick();  // 立即先查一轮
}

void MessageTab::stopPolling() {
	timer_->stop();
	stop_ = true;
	cancel_.store(true);  // 中止进行中的下载 (转写/总结无法中止, 只能等它收尾)
	jobCv_.notify_all();
	if (verifyThread_.joinable()) verifyThread_.join();
	if (pollThread_.joinable()) pollThread_.join();
	if (jobThread_.joinable()) jobThread_.join();
	{
		std::lock_guard<std::mutex> lock(jobMutex_);
		jobs_.clear();
	}

	if (running_) {
		running_ = false;
		appendLine(tr("已停止轮询"));
		stageLabel_->setText(tr("就绪"));
		emit statusChanged(tr("就绪"));
	}
}

// ---------------------------------------------------------------------------
// 轮询 (每拍一个短命线程, pollBusy_ 防重入)
// ---------------------------------------------------------------------------

void MessageTab::pollTick() {
	if (stop_) return;
	if (pollBusy_.exchange(true)) return;  // 上一轮还在跑 -> 跳过本拍
	if (pollThread_.joinable()) pollThread_.join();  // 上一轮已收尾, 立即返回
	pollThread_ = std::thread([this] {
		pollOnce();
		pollBusy_.store(false);
		});
}

void MessageTab::pollOnce() {
	// --- 通知消息 (message/msg.md: 未读计数 + @我的) ---
	// 新接口只给计数: 先取未读@数, 再用 msgfeed/at 取被@的通知内容。
	// 首轮无条件拉一次列表只建基线 (跳过已有历史), 之后仅在有未读@时才请求。
	{
		bili::MsgFeedUnread unread;
		std::string feedErr;
		if (!bili::fetchMsgFeedUnread(unread, feedErr)) {
			const QString q = QString::fromUtf8(feedErr);
			if (q != lastFeedErr_) {  // 连续同类错误只记一次
				emit logReady(QStringLiteral("获取未读通知失败: %1").arg(q));
				lastFeedErr_ = q;
			}
		}
		else {
			lastFeedErr_.clear();
			if (unread.at != lastAtUnread_) {  // 计数变化才上屏, 避免每拍刷屏
				emit logReady(QStringLiteral("未读@我的数量: %1").arg(unread.at));
				lastAtUnread_ = unread.at;
			}
			// msg.md 没有"标记已读"接口, 未读数会一直 > 0: 本轮是否拉列表只
			// 决定请求量, 真正防止重复处理的是下面的 noticeSeen_ 按 id 去重。
			// 内容取的是 @我的 通知, 所以只看 data.at 计数。
			if (unread.at > 0) {
				std::vector<bili::ReplyNotice> notices;
				if (!bili::fetchReplyNotices(notices, feedErr)) {
					const QString q = QString::fromUtf8(feedErr);
					if (q != lastFeedErr_) {
						emit logReady(QStringLiteral("获取@我的通知失败: %1").arg(q));
						lastFeedErr_ = q;
					}
				}
				else {
					lastFeedErr_.clear();
					// 未读@数 = 最新的 N 条 (B站打开@页整页标已读, 未读恒为最新一段),
					// 所以读取的 notices 只取前 data.at 条, 窗口外的视为已读。
					const size_t unreadAt =
						unread.at > 0 ? static_cast<size_t>(unread.at) : 0;
					std::vector<size_t> todo;  // 本轮要处理的通知下标

					for (size_t i = 0; i < notices.size(); ++i) {
						const bili::ReplyNotice& n = notices[i];
						if (stop_) return;
						if (n.id <= 0) continue;  // 无 id 无法去重, 直接丢弃
						QString atStr;
						for (const auto& u : n.atDetails)
						{
							atStr += QString("[mid:%1, nick:%2] ").arg(u.mid).arg(QString::fromStdString(u.nickname));
						}
						emit logReady(QStringLiteral("历史回复通知[仅建基线] id:%1, userMid:%2, nickname:%3, follow:%4, atTime:%5, title:%6, nativeUri:%7, uri:%8, business:%9, businessId:%10, detailTitle:%11, rootReplyContent:%12, sourceContent:%13, targetReplyContent:%14, atDetails:%15")
							.arg(n.id)
							.arg(n.userMid)
							.arg(QString::fromStdString(n.nickname).toHtmlEscaped())
							.arg(n.follow ? "true" : "false")
							.arg(n.atTime)
							.arg(QString::fromStdString(n.title).toHtmlEscaped())
							.arg(QString::fromStdString(n.nativeUri).toHtmlEscaped())
							.arg(QString::fromStdString(n.uri).toHtmlEscaped())
							.arg(QString::fromStdString(n.business).toHtmlEscaped())
							.arg(n.businessId)
							.arg(QString::fromStdString(n.detailTitle).toHtmlEscaped())
							.arg(QString::fromStdString(n.rootReplyContent).toHtmlEscaped())
							.arg(QString::fromStdString(n.sourceContent).toHtmlEscaped())
							.arg(QString::fromStdString(n.targetReplyContent).toHtmlEscaped())
							.arg(atStr.toHtmlEscaped())
						);
						if (noticeSeen_.count(n.id) != 0) continue;
						if (noticeSeen_.size() >= 4096) noticeSeen_.clear();  // 去重集合封顶
						if (i >= unreadAt) {  // 未读@窗口外 = 已读, 记为历史
							noticeSeen_.insert(n.id);
							continue;
						}
						if (n.userMid <= 0) {  // 没有 mid 无法回发私信
							noticeSeen_.insert(n.id);
							continue;
						}
						todo.push_back(i);
					}

					// 关注判断: msgfeed/at 的 user.follow 不可靠, 改用关系接口
					// (x/web-interface/relation) 逐个查"对方是否关注了我"。
					std::unordered_map<long long, bool> followsMe;
					if (!todo.empty()) {
						std::unordered_set<long long> uniq;
						std::vector<long long> mids;
						mids.reserve(todo.size());
						for (size_t k : todo) {
							const long long mid = notices[k].userMid;
							if (uniq.insert(mid).second) mids.push_back(mid);
						}
						std::string relErr;
						if (!bili::fetchRelations(mids, followsMe, relErr)) {
							const QString q = QString::fromUtf8(relErr);
							if (q != lastRelErr_) {  // 连续同类错误只记一次
								emit logReady(QStringLiteral("查询关注关系失败, 本轮跳过: %1").arg(q));
								lastRelErr_ = q;
							}
							todo.clear();  // 未写入 seen_, 下一轮重试
						}
						else {
							lastRelErr_.clear();
						}
					}

					for (size_t k : todo) {
						const bili::ReplyNotice& n = notices[k];
						if (stop_) return;
						noticeSeen_.insert(n.id);

						const auto rel = followsMe.find(n.userMid);
						if (rel == followsMe.end() || !rel->second) {
							emit logReady(QStringLiteral("@我的通知 %1 未关注我, 跳过: %2")
								.arg(n.userMid)
								.arg(contentPreview(QString::fromUtf8(n.title))));
							continue;
						}
						// 关注用户才解析 url: 去掉 #fragment 后按视频链接判别。
						std::string uri = n.uri;
						const size_t hash = uri.find('#');
						if (hash != std::string::npos) uri.erase(hash);
						if (uri.empty() || !bili::looksLikeBiliUrl(uri)) {
							emit logReady(QStringLiteral("@我的通知 %1 非视频链接, 跳过: %2")
								.arg(n.userMid)
								.arg(contentPreview(QString::fromUtf8(n.uri))));
							continue;
						}
						if (n.userMid <= 0) continue;
						if (stop_) return;

						// 取视频详情 (view API): 拿到 bvid/标题才认为是真实视频,
						// 并用规范地址 https://www.bilibili.com/video/<bvid> 入队。
						bili::VideoInfo vinfo;
						std::string resolveErr;
						if (!bili::resolve(uri, vinfo, resolveErr, bili::ProgressFn(), &cancel_)) {
							if (stop_) return;  // 停用引发的取消不当作失败
							emit logReady(QStringLiteral("获取 %1 的视频详情失败: %2")
								.arg(n.userMid)
								.arg(QString::fromUtf8(resolveErr)));
							continue;
						}
						std::string link;
						if (!vinfo.bvid.empty()) {
							link = "https://www.bilibili.com/video/" + vinfo.bvid;
						}
						else if (vinfo.aid > 0) {
							link = "https://www.bilibili.com/video/av" +
								std::to_string(vinfo.aid);
						}
						else {
							link = uri;  // PGC 等无 bvid/aid 时退回通知里的原始地址
						}

						enqueue(Job{ n.userMid, link });
						emit logReady(QStringLiteral("收到 %1 的视频《%2》: %3")
							.arg(n.userMid)
							.arg(QString::fromUtf8(vinfo.title))
							.arg(QString::fromUtf8(link)));
					}
				}
			}
		}
	}

	std::vector<bili::PmSession> sessions;
	std::string err;
	std::string ackErr;  // 设置已读的错误, 单独存放避免与 err 混用。
	if (!bili::fetchPmSessions(sessions, err)) {
		const QString qerr = QString::fromUtf8(err);
		if (qerr != lastPollErr_) {  // 连续同类错误只记一次, 避免刷屏
			emit logReady(qerr);
			lastPollErr_ = qerr;
		}
		return;
	}
	lastPollErr_.clear();

	for (const bili::PmSession& session : sessions) {
		if (stop_) return;
		long long begin = 0;
		const auto it = seqno_.find(session.talkerId);
		if (it == seqno_.end())
		{
			// 首见会话且有未读: 记录已读水位 ack_seqno 当起点, 之后只取消息
			// seqno 水位以后的部分 = 未读, 历史不会被当成新消息。
			// (无未读的会话不建条目, 等来新消息再建)
			if (session.unread <= 0) continue;
			begin = session.ackSeqno;
			seqno_[session.talkerId] = begin;  // 注意: 插入可能 rehash, 别再用 it
			emit logReady(QStringLiteral("会话 %1: 未读 %2 条, 从已读水位续传 ack_seqno=%3")
				.arg(session.talkerId)
				.arg(session.unread)
				.arg(begin));
		}
		else
		{
			begin = it->second;
		}

		std::vector<bili::PmMessage> msgs;
		if (!bili::fetchPmMessages(session.talkerId, begin, 1, msgs, err)) {
			emit logReady(QStringLiteral("获取 %1 的私信失败: %2")
				.arg(session.talkerId)
				.arg(QString::fromUtf8(err)));
			continue;
		}

		// 接口按 seqno 倒序返回: 从旧到新处理, 逐条推进基线。
		bool advanced = false;  // 本会话基线是否推进过 (推进过才需设已读)
		for (auto rit = msgs.rbegin(); rit != msgs.rend(); ++rit) {
			const bili::PmMessage& m = *rit;
			emit logReady(QStringLiteral("%1 发来消息: %2")
				.arg(session.talkerId)
				.arg(textPreview(m)));
			if (m.seqno <= seqno_[session.talkerId]) continue;
			seqno_[session.talkerId] = m.seqno;
			advanced = true;
			if (m.senderUid != session.talkerId || m.msgStatus != 0) continue;  // 自己的回声 / 已撤回

			const std::string link = bili::pmVideoLink(m);
			if (!link.empty()) {
				const std::string key =
					std::to_string(session.talkerId) + ":" + std::to_string(m.seqno);
				if (seen_.count(key) != 0) continue;
				if (seen_.size() >= 4096) seen_.clear();  // 去重集合封顶
				seen_.insert(key);
				enqueue(Job{ session.talkerId, link });
				emit logReady(QStringLiteral("收到 %1 的视频链接: %2")
					.arg(session.talkerId)
					.arg(QString::fromUtf8(link)));
			}
			else {
				// 所有非链接的收到消息都上屏 (文本给正文预览, 其它类型给原始内容预览)。
				emit logReady(QStringLiteral("%1 未解析发来消息: %2")
					.arg(session.talkerId)
					.arg(textPreview(m)));
			}
		}

		// 基线推进过 = 本轮读到了新消息: 设已读, 失败仅去重记日志, 不中断轮询。
		if (advanced) {
			if (!bili::markPmRead(session.talkerId, seqno_[session.talkerId], ackErr)) {
				const QString q = QString::fromUtf8(ackErr);
				if (q != lastAckErr_) { emit logReady(QStringLiteral("设置已读失败: %1").arg(q)); lastAckErr_ = q; }
			}
			else {
				emit logReady(QStringLiteral("设置已读: %1").arg(session.talkerId));
				lastAckErr_.clear();
			}
		}
	}
}

void MessageTab::enqueue(const Job& job) {
	{
		std::lock_guard<std::mutex> lock(jobMutex_);
		jobs_.push_back(job);
	}
	jobCv_.notify_one();
}

// ---------------------------------------------------------------------------
// 任务线程 (启用期间常驻)
// ---------------------------------------------------------------------------

void MessageTab::jobLoop() {
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(jobMutex_);
			jobCv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) return;
			job = jobs_.front();
			jobs_.pop_front();
		}
		processJob(job);
	}
}

void MessageTab::processJob(const Job& job) {
	emit logReady(QStringLiteral("开始处理 %1 的视频: %2")
		.arg(job.talkerId)
		.arg(QString::fromUtf8(job.link)));

	// 模型 / ollama / ffmpeg / 语言 / 输出目录均读取其它标签页写入的设置键。
	QSettings s;
	QString outDir = s.value(kKeyOutDir).toString();
	if (outDir.isEmpty()) {
		outDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	}
	if (outDir.isEmpty()) outDir = QDir::homePath();

	QString ffmpeg = s.value(kKeyFfmpeg).toString();
	if (ffmpeg.isEmpty()) ffmpeg = QString::fromStdWString(vsum::defaultFfmpegPath().wstring());
	QString model = s.value(kKeyModel).toString();
	if (model.isEmpty()) model = QString::fromStdWString(vsum::defaultModelPath().wstring());
	const QString language = s.value(kKeyLanguage, QStringLiteral("auto")).toString();
	const bool useGpu = s.value(kKeyUseGpu, true).toBool();
	QString ollamaUrl = s.value(kKeyOllamaUrl, QString::fromLatin1(kDefaultOllamaUrl)).toString();
	if (ollamaUrl.trimmed().isEmpty()) ollamaUrl = QString::fromLatin1(kDefaultOllamaUrl);
	QString ollamaModel =
		s.value(kKeyOllamaModel, QString::fromLatin1(kDefaultOllamaModel)).toString();
	if (ollamaModel.trimmed().isEmpty()) ollamaModel = QString::fromLatin1(kDefaultOllamaModel);

	const fs::path ffmpegPath(ffmpeg.toStdWString());

	cancel_.store(false);
	bili::VideoInfo info;
	fs::path outFile;
	std::string err;
	const bool dlOk = bili::download(
		job.link, fs::path(outDir.toStdWString()), ffmpegPath, info, outFile, err,
		[this](const std::string& stage, double fraction) {
			// 工作线程回调 -> 队列信号送回 GUI 线程
			emit progressReady(QString::fromUtf8(stage),
				fraction < 0.0 ? -1.0 : fraction * 100.0);
		},
		&cancel_);
	if (!dlOk) {
		failJob(job, QString::fromUtf8(info.title), QString::fromUtf8(err));
		return;
	}
	if (stop_) {
		emit logReady(QStringLiteral("已停止轮询, 跳过 %1 的后续处理").arg(job.talkerId));
		return;
	}

	emit progressReady(QStringLiteral("转写与总结"), -1.0);

	fs::path outPrefix = outFile;
	outPrefix.replace_extension();  // <标题>.mp4 -> 输出前缀 <标题>

	PipelineOptions po;
	po.input = outFile;
	po.outPrefix = outPrefix;
	po.model = fs::path(model.toStdWString());
	po.ffmpeg = ffmpegPath;
	po.language = language.toStdString();
	po.doSummary = true;
	po.useGpu = useGpu;
	po.ollamaUrl = ollamaUrl.toUtf8().toStdString();
	po.ollamaModel = ollamaModel.toUtf8().toStdString();

	const PipelineResult res = runPipeline(po,
		[this](const std::string& message, int percent) {
			emit progressReady(QString::fromUtf8(message), static_cast<double>(percent));
		});
	if (!res.ok) {
		const QString reason = res.error.empty()
			? QStringLiteral("转写管线退出码 %1").arg(res.exitCode)
			: QString::fromUtf8(res.error);
		failJob(job, QString::fromUtf8(info.title), reason);
		return;
	}
	if (stop_) {
		emit logReady(QStringLiteral("已停止轮询, 跳过回发 %1").arg(job.talkerId));
		return;
	}

	// 总结缺失/未生成时回退到带时间戳的字幕文本。
	std::string content = res.summary;
	if (content.empty() && !res.summaryPath.empty()) content = readFileUtf8(res.summaryPath);
	if (content.empty()) content = res.txt;
	if (content.empty()) content = "（未生成字幕与总结内容）";

	std::string sendErr;
	const std::string reply = "《" + info.title + "》\n\n" + content;
	if (bili::sendPmText(job.talkerId, reply, sendErr)) {
		emit logReady(QStringLiteral("已回复 %1: %2")
			.arg(job.talkerId)
			.arg(contentPreview(QString::fromUtf8(reply))));
	}
	else {
		emit logReady(QStringLiteral("回复 %1 失败: %2")
			.arg(job.talkerId)
			.arg(QString::fromUtf8(sendErr)));
	}
	emit progressReady(QStringLiteral("待命"), -1.0);
}

void MessageTab::failJob(const Job& job, const QString& title, const QString& reason) {
	emit logReady(QStringLiteral("处理失败: %1").arg(reason));
	if (stop_) return;  // 停用/关闭引发的失败不打扰对方
	const QString t = title.isEmpty() ? QStringLiteral("未知标题") : title;
	const std::string msg = "视频《" + t.toUtf8().toStdString() + "》处理失败：" +
		reason.toUtf8().toStdString();
	std::string sendErr;
	if (bili::sendPmText(job.talkerId, msg, sendErr)) {
		emit logReady(QStringLiteral("已回发失败通知 %1: %2")
			.arg(job.talkerId)
			.arg(contentPreview(QString::fromUtf8(msg))));
	}
	else {
		emit logReady(QStringLiteral("失败通知回发 %1 也失败: %2")
			.arg(job.talkerId)
			.arg(QString::fromUtf8(sendErr)));
	}
}

// ---------------------------------------------------------------------------
// GUI 线程回调 (队列连接)
// ---------------------------------------------------------------------------

void MessageTab::onIntervalChanged(int value) {
	if (timer_->isActive()) timer_->setInterval(value * 1000);
	saveSettings();
}

void MessageTab::onClearLog() { log_->clear(); }

void MessageTab::onLogLine(const QString& line) { appendLine(line); }

void MessageTab::onProgress(const QString& stage, double percent) {
	if (stage.isEmpty()) return;
	if (percent < 0.0) {
		stageLabel_->setText(stage);
	}
	else {
		stageLabel_->setText(QStringLiteral("%1 %2%").arg(stage).arg(qRound(percent)));
	}
}

void MessageTab::appendLine(const QString& text) {
	vsum::appendLog(log_, QStringLiteral("[%1] %2")
		.arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text));
}
