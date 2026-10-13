/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayView.h"
#include "moc_NetplayView.cpp"

#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>

#include "ConfigController.h"
#include "CoreController.h"
#include "GBAApp.h"
#include "NetplayController.h"
#include "Window.h"
#include "utils.h"

#include <mgba/feature/netplay-net.h>

using namespace QGBA;

static const char* SETTINGS_GROUP = "netplay";

NetplayView::NetplayView(Window* window, QWidget* parent)
	: QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint)
	, m_window(window)
	, m_netplay(window->netplay())
{
	m_ui.setupUi(this);

	ConfigController* config = window->config();
	m_ui.romPath->setText(config->getQtOption("rom", SETTINGS_GROUP).toString());
	m_ui.address->setText(config->getQtOption("address", SETTINGS_GROUP).toString());
	m_ui.port->setValue(config->getQtOption("port", SETTINGS_GROUP).toInt() ? config->getQtOption("port", SETTINGS_GROUP).toInt() : mNETPLAY_DEFAULT_PORT);
	m_ui.players->setValue(config->getQtOption("players", SETTINGS_GROUP).toInt() ? config->getQtOption("players", SETTINGS_GROUP).toInt() : 2);
	if (config->getQtOption("hosting", SETTINGS_GROUP).toBool()) {
		m_ui.tabs->setCurrentWidget(m_ui.hostTab);
	}

	connect(window, &QObject::destroyed, this, &QWidget::close);
	connect(m_ui.browse, &QAbstractButton::clicked, this, &NetplayView::browse);
	connect(m_ui.host, &QAbstractButton::clicked, this, &NetplayView::host);
	connect(m_ui.join, &QAbstractButton::clicked, this, &NetplayView::join);
	connect(m_ui.address, &QLineEdit::returnPressed, this, &NetplayView::join);
	connect(m_ui.leave, &QAbstractButton::clicked, m_netplay, &NetplayController::leave);
	connect(m_ui.close, &QAbstractButton::clicked, this, &QWidget::close);
	connect(m_netplay, &NetplayController::statusChanged, this, &NetplayView::updateStatus);
	connect(m_netplay, &NetplayController::finished, this, &NetplayView::finished);

	updateStatus();
}

void NetplayView::browse() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), romFilters());
	if (!filename.isEmpty()) {
		m_ui.romPath->setText(filename);
	}
}

void NetplayView::remember() {
	ConfigController* config = m_window->config();
	config->setQtOption("rom", m_ui.romPath->text(), SETTINGS_GROUP);
	config->setQtOption("address", m_ui.address->text(), SETTINGS_GROUP);
	config->setQtOption("port", m_ui.port->value(), SETTINGS_GROUP);
	config->setQtOption("players", m_ui.players->value(), SETTINGS_GROUP);
	config->setQtOption("hosting", m_ui.tabs->currentWidget() == m_ui.hostTab, SETTINGS_GROUP);
}

void NetplayView::host() {
	remember();
	m_netplay->host(m_ui.romPath->text(), m_ui.port->value(), m_ui.players->value());
}

void NetplayView::join() {
	remember();
	m_netplay->join(m_ui.romPath->text(), m_ui.address->text());
}

QString NetplayView::localAddresses() {
	QStringList addresses;
	for (const QHostAddress& address : QNetworkInterface::allAddresses()) {
		if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) {
			continue;
		}
		// Tailscale hands out addresses in 100.64.0.0/10
		if (address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)) {
			addresses.prepend(tr("%1 (Tailscale)").arg(address.toString()));
		} else {
			addresses.append(address.toString());
		}
	}
	return addresses.join(QStringLiteral(", "));
}

void NetplayView::updateStatus() {
	NetplayController::Status status = m_netplay->status();
	bool busy = status.phase != NetplayController::Phase::IDLE && status.phase != NetplayController::Phase::OVER;
	m_ui.host->setEnabled(!busy);
	m_ui.join->setEnabled(!busy);
	m_ui.romPath->setEnabled(!busy);
	m_ui.browse->setEnabled(!busy);
	m_ui.leave->setEnabled(busy);

	QString text;
	QString title;
	switch (status.phase) {
	case NetplayController::Phase::IDLE:
		break;
	case NetplayController::Phase::CONNECTING:
		text = tr("Connecting...");
		break;
	case NetplayController::Phase::LOBBY:
		if (status.localSlot == 0) {
			text = tr("Hosting: %1 of %2 players are here. The game starts when everyone has joined.\n"
			          "Friends join with your address and port %3. Your addresses: %4")
			       .arg(status.joined).arg(status.players).arg(m_ui.port->value()).arg(localAddresses());
		} else {
			text = tr("Joined as player %1: %2 of %3 players are here. The game starts when everyone has joined.")
			       .arg(status.localSlot + 1).arg(status.joined).arg(status.players);
		}
		break;
	case NetplayController::Phase::PLAYING:
		text = tr("Playing as player %1 of %2. Input delay: %3 frames (%4 ms). Round trip: %5 ms.")
		       .arg(status.localSlot + 1).arg(status.players).arg(status.inputDelay)
		       .arg(status.inputDelay * 1000 / 60).arg(status.rttMs);
		if (status.resyncs) {
			text += tr(" Resynced %n time(s).", nullptr, status.resyncs);
		}
		title = tr("Netplay P%1/%2, %3 ms").arg(status.localSlot + 1).arg(status.players).arg(status.rttMs);
		break;
	case NetplayController::Phase::OVER:
		text = status.message;
		break;
	}
	m_ui.status->setText(text);
	m_window->setNetplayStatus(title);
	if (status.phase == NetplayController::Phase::PLAYING && m_lastPhase != NetplayController::Phase::PLAYING) {
		// Out of the way of the game; it comes back when the game ends
		hide();
	}
	m_lastPhase = status.phase;

	// A stall a player can notice gets a note on screen naming who is late.
	bool waiting = status.phase == NetplayController::Phase::PLAYING && status.waitingMask && status.stalledMs >= 250;
	if (waiting && !m_waitingShown) {
		QStringList players;
		for (unsigned slot = 0; slot < status.players; ++slot) {
			if (status.waitingMask & (1 << slot)) {
				players.append(QString::number(slot + 1));
			}
		}
		std::shared_ptr<CoreController> controller = m_window->controller();
		if (controller) {
			emit controller->statusPosted(tr("Waiting for player %1...").arg(players.join(QStringLiteral(", "))));
		}
	}
	m_waitingShown = waiting;
}

void NetplayView::finished(const QString& message) {
	updateStatus();
	if (!message.isEmpty()) {
		m_ui.status->setText(message);
		show();
		raise();
	}
}
