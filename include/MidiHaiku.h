/*
 * MidiHaiku.h - MIDI client using Haiku's native MIDI Kit (midi2)
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_MIDI_HAIKU_H
#define LMMS_MIDI_HAIKU_H

#include "lmmsconfig.h"

#ifdef LMMS_BUILD_HAIKU

#include <memory>

#include <QObject>

#include "MidiClient.h"
#include "MidiPort.h"


namespace lmms
{

/*! \brief MIDI client for Haiku, built on the MIDI Kit (libmidi2).

	The producers (sources) and consumers (sinks) registered with Haiku's
	MIDI roster - hardware ports, software synths and other applications -
	are offered as readable and writable ports. Subscribing a MidiPort to one
	of them connects a small endpoint owned by LMMS to it; these endpoints
	show up in Haiku's MIDI patchbay as "LMMS <- name" and "LMMS -> name".

	The Haiku headers are only included by MidiHaiku.cpp, so that their
	macros and type names stay out of the rest of LMMS.
*/
class MidiHaiku : public QObject, public MidiClient
{
	Q_OBJECT
public:
	MidiHaiku();
	~MidiHaiku() override;

	//! false if the MIDI server could not be reached
	bool isRunning() const;

	static QString probeDevice()
	{
		return QString(); // no device name to configure
	}

	static QString name()
	{
		return QT_TRANSLATE_NOOP("MidiSetupWidget", "Haiku MIDI");
	}

	static QString configSection()
	{
		return QString(); // no configuration settings
	}

	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port) override;

	void applyPortMode(MidiPort* port) override;
	void removePort(MidiPort* port) override;

	QStringList readablePorts() const override;
	QStringList writablePorts() const override;

	QString sourcePortName(const MidiEvent& event) const override;

	void subscribeReadablePort(MidiPort* port, const QString& dest, bool subscribe = true) override;
	void subscribeWritablePort(MidiPort* port, const QString& dest, bool subscribe = true) override;

	void connectRPChanged(QObject* receiver, const char* member) override
	{
		connect(this, SIGNAL(readablePortsChanged()), receiver, member);
	}

	void connectWPChanged(QObject* receiver, const char* member) override
	{
		connect(this, SIGNAL(writablePortsChanged()), receiver, member);
	}

signals:
	void readablePortsChanged();
	void writablePortsChanged();

private slots:
	//! re-reads the roster; queued from the roster watcher's thread
	void refreshEndpoints();

private:
	struct Impl;

	//! unsubscribes a port (from one destination, or from all if none is given)
	void removeFromInputs(MidiPort* port, const QString& dest = QString());
	void removeFromOutputs(MidiPort* port, const QString& dest = QString());

	std::unique_ptr<Impl> m_impl;
};

} // namespace lmms

#endif // LMMS_BUILD_HAIKU

#endif // LMMS_MIDI_HAIKU_H
