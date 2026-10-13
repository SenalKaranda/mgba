/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <cstdint>

struct mCore;
struct mNetplayLink;
struct mNetplayPeer;

class QThread;

namespace QGBA {

class CoreController;
class Window;

// Plays one player of an internet game. Every player's PC emulates all the
// linked consoles (a netplay session), and only button presses cross the
// network. In the lobby the peer is polled from the GUI thread; once the
// game starts, the local player's console runs in a normal CoreController
// whose core thread drives the whole session, so the window shows and
// plays it like any other game.
class NetplayController : public QObject {
Q_OBJECT

public:
	enum class Phase {
		IDLE,
		CONNECTING,
		LOBBY,
		PLAYING,
		OVER,
	};

	struct Status {
		Phase phase = Phase::IDLE;
		unsigned players = 0;
		unsigned joined = 0;
		unsigned localSlot = 0;
		unsigned inputDelay = 0;
		unsigned rttMs = 0;
		uint32_t frame = 0;
		// Players whose keys are late, while a stall lasts.
		uint32_t waitingMask = 0;
		uint64_t stalledMs = 0;
		unsigned stalls = 0;
		unsigned resyncs = 0;
		QString message;
	};

	NetplayController(Window* window, QObject* parent = nullptr);
	~NetplayController();

	bool host(const QString& romPath, unsigned port, unsigned players);
	bool join(const QString& romPath, const QString& address);
	// Leaves the lobby, or ends the game for everyone.
	void leave();

	Status status() const;
	bool isBusy() const;
	QString savePath() const { return m_savePath; }

signals:
	void statusChanged();
	// The lobby or the game is over; the message says why.
	void finished(const QString& message);

private slots:
	void pollLobby();
	void connectFinished();
	void gameOver();
	void gameStopped();
	void writeSave(const QByteArray& save);

private:
	bool prepare(const QString& romPath);
	bool startPeer(mNetplayLink* link);
	void startGame();
	void finish(const QString& message);
	void destroyPeer();
	void backUpSave();

	// Called on the core thread while playing.
	void stepOnCoreThread();
	void publish(uint64_t nowNs, bool force);
	void saveFromCoreThread();

	static void prepareCore(void* context, mCore* core, unsigned slot, bool local);
	static void coreRunLoop(mCore* core);
	static void coreReset(mCore* core);
	static void savedataUpdated(void* context);

	Window* m_window;
	QTimer m_lobbyTimer;
	QThread* m_connectThread = nullptr;
	mNetplayLink* m_connectedLink = nullptr;
	QString m_connectHost;
	unsigned m_connectPort = 0;

	mNetplayPeer* m_peer = nullptr;
	bool m_hosting = false;
	unsigned m_players = 0;
	QByteArray m_rom;
	QByteArray m_save;
	QString m_romPath;
	QString m_savePath;
	bool m_backedUp = false;
	QByteArray m_firstFrameBuffer;

	// Owned by the Window. The plain pointer is for the core thread, which
	// only runs while the controller exists.
	QPointer<CoreController> m_controller;
	CoreController* m_playingController = nullptr;
	mCore* m_localCore = nullptr;

	// Core thread only.
	uint64_t m_nextFrameNs = 0;
	uint64_t m_stallStartNs = 0;
	uint64_t m_lastPublishNs = 0;
	bool m_overSignalled = false;

	mutable QMutex m_statusMutex;
	Status m_status;
};

}
