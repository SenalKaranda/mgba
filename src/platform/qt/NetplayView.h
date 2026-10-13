/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "ui_NetplayView.h"

#include "NetplayController.h"

namespace QGBA {

class Window;

class NetplayView : public QDialog {
Q_OBJECT

public:
	NetplayView(Window* window, QWidget* parent = nullptr);

public slots:
	void host();
	void join();

private slots:
	void browse();
	void updateStatus();
	void finished(const QString& message);

private:
	void remember();
	static QString localAddresses();

	Ui::NetplayView m_ui;
	Window* m_window;
	NetplayController* m_netplay;
	bool m_waitingShown = false;
	NetplayController::Phase m_lastPhase = NetplayController::Phase::IDLE;
};

}
