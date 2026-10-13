/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayController.h"

#include "moc_NetplayController.cpp"

#include "ConfigController.h"
#include "CoreController.h"
#include "Window.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/feature/netplay-net.h>

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

using namespace QGBA;

namespace {

// One GBA frame: 280896 cycles of the 16.78 MHz clock.
constexpr uint64_t FRAME_NS = 16742706;
// Further behind than this (after a stall, or a hiccup on this PC), the
// game resumes from now instead of racing to catch up.
constexpr uint64_t MAX_CATCH_UP_NS = 3 * FRAME_NS;
constexpr uint64_t PUBLISH_NS = 100000000;
constexpr unsigned CONNECT_TIMEOUT_MS = 10000;
constexpr int LOBBY_POLL_MS = 5;
constexpr int MAX_ROM_SIZE = 32 * 1024 * 1024;

// The controller whose game is running; only one netplay game at a time.
NetplayController* s_playing = nullptr;

uint64_t monotonicNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

NetplayController::NetplayController(Window* window, QObject* parent)
	: QObject(parent)
	, m_window(window)
{
	m_lobbyTimer.setInterval(LOBBY_POLL_MS);
	connect(&m_lobbyTimer, &QTimer::timeout, this, &NetplayController::pollLobby);
}

NetplayController::~NetplayController() {
	m_lobbyTimer.stop();
	if (m_connectThread) {
		m_connectThread->wait();
		delete m_connectThread;
		if (m_connectedLink) {
			m_connectedLink->destroy(m_connectedLink);
		}
	}
	if (m_playingController) {
		// The window is going away with the game still running; its core
		// belongs to the peer, so leave both to the end of the process
		// rather than pull the session out from under the core thread.
		s_playing = nullptr;
		return;
	}
	destroyPeer();
}

NetplayController::Status NetplayController::status() const {
	QMutexLocker locker(&m_statusMutex);
	return m_status;
}

bool NetplayController::isBusy() const {
	QMutexLocker locker(&m_statusMutex);
	return m_status.phase != Phase::IDLE && m_status.phase != Phase::OVER;
}

bool NetplayController::prepare(const QString& romPath) {
	if (isBusy() || s_playing) {
		return false;
	}
	QFileInfo info(romPath);
	QString suffix = info.suffix().toLower();
	if (suffix == "zip" || suffix == "7z" || suffix == "rar") {
		finish(tr("Netplay needs the ROM itself, not an archive; please extract it first."));
		return false;
	}
	QFile rom(romPath);
	if (!rom.open(QIODevice::ReadOnly) || rom.size() > MAX_ROM_SIZE) {
		finish(tr("Could not read the ROM %1").arg(romPath));
		return false;
	}
	m_rom = rom.readAll();
	m_romPath = info.absoluteFilePath();

	// The same save the game would use when played alone.
	QString saveDir = m_window->config()->getOption("savegamePath");
	if (saveDir.isEmpty()) {
		saveDir = info.absolutePath();
	}
	m_savePath = QDir(saveDir).filePath(info.completeBaseName() + ".sav");
	m_save.clear();
	QFile save(m_savePath);
	if (save.exists()) {
		if (!save.open(QIODevice::ReadOnly)) {
			finish(tr("Could not read your save %1").arg(m_savePath));
			return false;
		}
		m_save = save.readAll();
	}
	m_backedUp = false;
	m_overSignalled = false;
	m_nextFrameNs = 0;
	m_stallStartNs = 0;
	m_lastPublishNs = 0;

	QMutexLocker locker(&m_statusMutex);
	m_status = Status();
	return true;
}

static mNetplayPeerConfig peerConfig(const QByteArray& rom, const QByteArray& save) {
	mNetplayPeerConfig config{};
	config.rom = rom.constData();
	config.romSize = rom.size();
	config.save = save.isEmpty() ? nullptr : save.constData();
	config.saveSize = save.size();
	return config;
}

bool NetplayController::host(const QString& romPath, unsigned port, unsigned players) {
	if (!prepare(romPath)) {
		return false;
	}
	mNetplayListener* listener = mNetplaySocketListen(port);
	if (!listener) {
		finish(tr("Could not open port %1 (TCP and UDP); is something else using it?").arg(port));
		return false;
	}
	mNetplayPeerConfig config = peerConfig(m_rom, m_save);
	config.players = players;
	config.rtcEpochMs = QDateTime::currentMSecsSinceEpoch();
	config.prepareCore = prepareCore;
	config.context = this;
	m_peer = mNetplayPeerCreateHost(&config, listener, mNetplaySocketClock());
	if (!m_peer) {
		listener->destroy(listener);
		finish(tr("Could not start hosting"));
		return false;
	}
	m_hosting = true;
	m_players = players;
	{
		QMutexLocker locker(&m_statusMutex);
		m_status.phase = Phase::LOBBY;
		m_status.players = players;
		m_status.joined = 1;
	}
	m_lobbyTimer.start();
	emit statusChanged();
	return true;
}

bool NetplayController::join(const QString& romPath, const QString& address) {
	QString host = address.trimmed();
	unsigned port = mNETPLAY_DEFAULT_PORT;
	int colon = host.lastIndexOf(':');
	if (colon > 0) {
		bool ok = false;
		port = host.mid(colon + 1).toUInt(&ok);
		host = host.left(colon);
		if (!ok || !port || port > 65535) {
			finish(tr("%1 is not a valid address; use HOST or HOST:PORT").arg(address));
			return false;
		}
	}
	if (host.isEmpty()) {
		finish(tr("Enter the host's address"));
		return false;
	}
	if (!prepare(romPath)) {
		return false;
	}
	m_hosting = false;
	m_connectHost = host;
	m_connectPort = port;
	m_connectedLink = nullptr;
	{
		QMutexLocker locker(&m_statusMutex);
		m_status.phase = Phase::CONNECTING;
	}
	// Name lookup and connecting block, so they happen off the GUI thread.
	QByteArray hostName = host.toUtf8();
	m_connectThread = QThread::create([this, hostName, port]() {
		m_connectedLink = mNetplaySocketConnect(hostName.constData(), port, CONNECT_TIMEOUT_MS);
	});
	connect(m_connectThread, &QThread::finished, this, &NetplayController::connectFinished);
	m_connectThread->start();
	emit statusChanged();
	return true;
}

void NetplayController::connectFinished() {
	m_connectThread->wait();
	delete m_connectThread;
	m_connectThread = nullptr;
	mNetplayLink* link = m_connectedLink;
	m_connectedLink = nullptr;
	if (status().phase != Phase::CONNECTING) {
		// Left while connecting
		if (link) {
			link->destroy(link);
		}
		return;
	}
	if (!link) {
		finish(tr("Could not reach %1 on port %2. Is the host waiting, and can this PC reach it (Tailscale running, or the port forwarded)?")
		       .arg(m_connectHost).arg(m_connectPort));
		return;
	}
	mNetplayPeerConfig config = peerConfig(m_rom, m_save);
	config.prepareCore = prepareCore;
	config.context = this;
	m_peer = mNetplayPeerCreateClient(&config, link, mNetplaySocketClock());
	if (!m_peer) {
		link->destroy(link);
		finish(tr("Could not join the game"));
		return;
	}
	m_lobbyTimer.start();
}

void NetplayController::pollLobby() {
	if (!m_peer) {
		m_lobbyTimer.stop();
		return;
	}
	mNetplayPeerPoll(m_peer);
	switch (mNetplayPeerGetState(m_peer)) {
	case mNETPLAY_PEER_CONNECTING:
	case mNETPLAY_PEER_LOBBY: {
		mNetplayPeerStats stats;
		mNetplayPeerGetStats(m_peer, &stats);
		Status before = status();
		{
			QMutexLocker locker(&m_statusMutex);
			m_status.phase = mNetplayPeerGetState(m_peer) == mNETPLAY_PEER_LOBBY ? Phase::LOBBY : Phase::CONNECTING;
			m_status.players = mNetplayPeerPlayers(m_peer);
			m_status.joined = mNetplayPeerJoined(m_peer);
			m_status.localSlot = mNetplayPeerLocalSlot(m_peer);
			m_status.rttMs = stats.rttMs;
		}
		Status after = status();
		if (before.phase != after.phase || before.players != after.players || before.joined != after.joined || before.rttMs != after.rttMs) {
			emit statusChanged();
		}
		break;
	}
	case mNETPLAY_PEER_RUNNING:
		m_lobbyTimer.stop();
		startGame();
		break;
	case mNETPLAY_PEER_ENDED:
	case mNETPLAY_PEER_FAILED:
		m_lobbyTimer.stop();
		finish(QString::fromUtf8(mNetplayPeerError(m_peer)));
		destroyPeer();
		break;
	}
}

void NetplayController::startGame() {
	mNetplaySession* session = mNetplayPeerSession(m_peer);
	unsigned slot = mNetplayPeerLocalSlot(m_peer);
	m_localCore = mNetplaySessionGetCore(session, slot);
	backUpSave();

	// From here on the core thread drives the session: the window's
	// thread calls this core's runLoop, which steps every console.
	m_localCore->runLoop = coreRunLoop;
	m_localCore->reset = coreReset;
	s_playing = this;

	CoreController* controller = new CoreController(m_localCore);
	controller->setNetplay(true);
	QFileInfo info(m_romPath);
	controller->setPath(m_romPath, info.absolutePath());
	m_playingController = controller;
	m_controller = controller;
	connect(controller, &CoreController::stopping, this, &NetplayController::gameStopped);

#ifdef _WIN32
	// 1 ms sleeps, so frames start on time
	timeBeginPeriod(1);
#endif
	{
		QMutexLocker locker(&m_statusMutex);
		m_status.phase = Phase::PLAYING;
		m_status.localSlot = slot;
		m_status.players = mNetplayPeerPlayers(m_peer);
		m_status.joined = m_status.players;
		m_status.inputDelay = mNetplayPeerInputDelay(m_peer);
	}
	emit statusChanged();
	m_window->setController(controller);
}

void NetplayController::leave() {
	switch (status().phase) {
	case Phase::CONNECTING:
		// connectFinished() drops the connection when it arrives.
		finish(tr("Left the game"));
		break;
	case Phase::LOBBY:
		m_lobbyTimer.stop();
		destroyPeer();
		finish(tr("Left the game"));
		break;
	case Phase::PLAYING:
		if (m_controller) {
			{
				QMutexLocker locker(&m_statusMutex);
				m_status.message = tr("You left the game");
			}
			m_controller->stop();
		}
		break;
	case Phase::IDLE:
	case Phase::OVER:
		break;
	}
}

void NetplayController::gameOver() {
	// The game ended on the network side (finished, or someone dropped);
	// stop the console, and gameStopped() cleans up once its loop is done.
	if (m_controller) {
		m_controller->stop();
	}
}

void NetplayController::gameStopped() {
	if (!m_playingController) {
		return;
	}
	// The core thread has left its loop, so nothing steps the session any
	// more. The window keeps using its console (the last picture, its size)
	// until it lets go of the controller, so the session lives until then;
	// the network side ends now, so the others hear of it straight away.
	CoreController* controller = m_playingController;
	m_playingController = nullptr;
	s_playing = nullptr;
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	mNetplaySession* session = m_peer ? mNetplayPeerTakeSession(m_peer) : nullptr;
	if (session) {
		void* data = nullptr;
		size_t size = mNetplaySessionCopySave(session, mNetplayPeerLocalSlot(m_peer), &data);
		if (data) {
			QByteArray save(static_cast<const char*>(data), size);
			free(data);
			if (save != m_save) {
				writeSave(save);
			}
		}
		connect(controller, &QObject::destroyed, [session]() {
			mNetplaySessionDestroy(session);
		});
	}
	QString message = status().message;
	destroyPeer();
	finish(message.isEmpty() ? tr("The game is over") : message);
}

void NetplayController::finish(const QString& message) {
	{
		QMutexLocker locker(&m_statusMutex);
		m_status.phase = Phase::OVER;
		m_status.message = message;
		m_status.waitingMask = 0;
		m_status.stalledMs = 0;
	}
	emit statusChanged();
	emit finished(message);
}

void NetplayController::destroyPeer() {
	if (m_peer) {
		mNetplayPeerDestroy(m_peer);
		m_peer = nullptr;
	}
	m_localCore = nullptr;
}

void NetplayController::backUpSave() {
	if (m_backedUp || m_save.isEmpty()) {
		return;
	}
	QString backup = QString("%1.netplay-%2.bak").arg(m_savePath).arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
	m_backedUp = QFile::copy(m_savePath, backup);
}

void NetplayController::writeSave(const QByteArray& save) {
	if (save.isEmpty() || m_savePath.isEmpty()) {
		return;
	}
	backUpSave();
	QSaveFile file(m_savePath);
	if (!file.open(QIODevice::WriteOnly) || file.write(save) != save.size() || !file.commit()) {
		if (m_controller) {
			emit m_controller->statusPosted(tr("Could not write your save to %1").arg(m_savePath));
		}
		return;
	}
	m_save = save;
}

// --- Core thread ------------------------------------------------------------

void NetplayController::prepareCore(void* context, mCore* core, unsigned, bool local) {
	if (!local) {
		return;
	}
	NetplayController* self = static_cast<NetplayController*>(context);
	// A video buffer before the first reset gets this console a real
	// renderer; the CoreController swaps in its own buffer later.
	unsigned width, height;
	core->baseVideoSize(core, &width, &height);
	self->m_firstFrameBuffer.fill(0, width * height * sizeof(mColor));
	core->setVideoBuffer(core, reinterpret_cast<mColor*>(self->m_firstFrameBuffer.data()), width);

	mCoreCallbacks callbacks{};
	callbacks.context = self;
	callbacks.savedataUpdated = savedataUpdated;
	core->addCoreCallbacks(core, &callbacks);
}

void NetplayController::coreRunLoop(mCore*) {
	if (!s_playing) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return;
	}
	s_playing->stepOnCoreThread();
}

void NetplayController::coreReset(mCore*) {
	// A reset of one console would split it from the others. The session
	// reset every console before the game started.
}

void NetplayController::savedataUpdated(void* context) {
	static_cast<NetplayController*>(context)->saveFromCoreThread();
}

void NetplayController::saveFromCoreThread() {
	// The game just finished writing its save: keep this player's on disk.
	mNetplaySession* session = mNetplayPeerSession(m_peer);
	void* data = nullptr;
	size_t size = mNetplaySessionCopySave(session, mNetplayPeerLocalSlot(m_peer), &data);
	if (!data) {
		return;
	}
	QByteArray save(static_cast<const char*>(data), size);
	free(data);
	QMetaObject::invokeMethod(this, "writeSave", Qt::QueuedConnection, Q_ARG(QByteArray, save));
}

void NetplayController::stepOnCoreThread() {
	uint64_t now = monotonicNs();
	mNetplayPeerPoll(m_peer);
	if (mNetplayPeerGetState(m_peer) != mNETPLAY_PEER_RUNNING) {
		if (!m_overSignalled) {
			m_overSignalled = true;
			QString message = QString::fromUtf8(mNetplayPeerError(m_peer));
			if (message.isEmpty()) {
				message = tr("The game is over");
			}
			{
				QMutexLocker locker(&m_statusMutex);
				if (m_status.message.isEmpty()) {
					m_status.message = message;
				}
			}
			QMetaObject::invokeMethod(this, "gameOver", Qt::QueuedConnection);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return;
	}

	if (!m_nextFrameNs || now - m_nextFrameNs > MAX_CATCH_UP_NS) {
		m_nextFrameNs = now;
	}
	if (now < m_nextFrameNs) {
		// Early: keep the network moving until the frame is due.
		uint64_t wait = std::min<uint64_t>(m_nextFrameNs - now, 1000000);
		std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
		publish(now, false);
		return;
	}

	int keys = m_playingController->netplayKeys() & mNETPLAY_KEY_MASK;
	switch (mNetplayPeerRunFrame(m_peer, keys)) {
	case mNETPLAY_PEER_STEP_OK:
		m_nextFrameNs += FRAME_NS;
		if (m_stallStartNs) {
			m_stallStartNs = 0;
			publish(now, true);
		}
		break;
	case mNETPLAY_PEER_STEP_STALLED:
		// Someone's keys are late; the frame waits for them.
		if (!m_stallStartNs) {
			m_stallStartNs = now;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(500));
		break;
	default:
		break;
	}
	publish(now, false);
}

void NetplayController::publish(uint64_t nowNs, bool force) {
	if (!force && nowNs - m_lastPublishNs < PUBLISH_NS) {
		return;
	}
	m_lastPublishNs = nowNs;
	mNetplayPeerStats stats;
	mNetplayPeerGetStats(m_peer, &stats);
	{
		QMutexLocker locker(&m_statusMutex);
		m_status.frame = mNetplayPeerFrame(m_peer);
		m_status.inputDelay = mNetplayPeerInputDelay(m_peer);
		m_status.rttMs = stats.rttMs;
		m_status.stalls = stats.stalls;
		m_status.resyncs = stats.resyncs;
		m_status.waitingMask = m_stallStartNs ? mNetplayPeerWaitingMask(m_peer) : 0;
		m_status.stalledMs = m_stallStartNs ? (nowNs - m_stallStartNs) / 1000000 : 0;
	}
	QMetaObject::invokeMethod(this, "statusChanged", Qt::QueuedConnection);
}
