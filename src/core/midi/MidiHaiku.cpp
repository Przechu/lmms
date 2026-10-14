/*
 * MidiHaiku.cpp - MIDI client using Haiku's native MIDI Kit (midi2)
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

#include "MidiHaiku.h"

#ifdef LMMS_BUILD_HAIKU

#include <algorithm>
#include <atomic>

#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <MidiConsumer.h>
#include <MidiProducer.h>
#include <MidiRoster.h>
#include <OS.h>

#include "MidiPort.h"


namespace lmms
{

namespace
{

constexpr int kControllerAllNotesOff = 123;

//! Receives the events of the endpoints below, on their own threads.
class EventSink
{
public:
	virtual ~EventSink() = default;
	virtual void deliver(const QString& source, const void* endpoint, const MidiEvent& event) = 0;
};


/*! An endpoint that LMMS registers for one remote producer. The producer is
	connected to it, so everything it plays arrives here. */
class InEndpoint : public BMidiLocalConsumer
{
public:
	InEndpoint(EventSink& sink, const QString& source) :
		BMidiLocalConsumer(QString("LMMS <- %1").arg(source).toUtf8().constData()),
		m_sink(sink),
		m_source(source)
	{
	}

	void NoteOff(uchar channel, uchar note, uchar velocity, bigtime_t) override
	{
		send(MidiNoteOff, channel, note, velocity);
	}

	void NoteOn(uchar channel, uchar note, uchar velocity, bigtime_t) override
	{
		// a note on with velocity 0 is a note off
		send(velocity ? MidiNoteOn : MidiNoteOff, channel, note, velocity);
	}

	void KeyPressure(uchar channel, uchar note, uchar pressure, bigtime_t) override
	{
		send(MidiKeyPressure, channel, note, pressure);
	}

	void ControlChange(uchar channel, uchar controller, uchar value, bigtime_t) override
	{
		send(MidiControlChange, channel, controller, value);
	}

	void ProgramChange(uchar channel, uchar program, bigtime_t) override
	{
		send(MidiProgramChange, channel, program, 0);
	}

	void ChannelPressure(uchar channel, uchar pressure, bigtime_t) override
	{
		send(MidiChannelPressure, channel, pressure, 0);
	}

	void PitchBend(uchar channel, uchar lsb, uchar msb, bigtime_t) override
	{
		// LMMS keeps the bend as one 14 bit value, 8192 being the middle
		send(MidiPitchBend, channel, (lsb & 0x7f) | ((msb & 0x7f) << 7), 0);
	}

	void AllNotesOff(bool, bigtime_t) override
	{
		// the channel of an all-notes-off message is not passed on to us
		for (int channel = 0; channel < 16; ++channel)
		{
			send(MidiControlChange, channel, kControllerAllNotesOff, 0);
		}
	}

private:
	void send(MidiEventTypes type, int channel, int param1, int param2)
	{
		m_sink.deliver(m_source, this, MidiEvent(type, channel & 0x0f, param1, param2, this));
	}

	EventSink& m_sink;
	const QString m_source;
};


//! An endpoint that LMMS registers for one remote consumer, which is connected to it.
class OutEndpoint : public BMidiLocalProducer
{
public:
	explicit OutEndpoint(const QString& dest) :
		BMidiLocalProducer(QString("LMMS -> %1").arg(dest).toUtf8().constData())
	{
	}
};


//! Forwards the roster's change notifications to the client, on the Qt side.
class RosterWatcher : public BLooper
{
public:
	explicit RosterWatcher(QObject* client) :
		BLooper("LMMS MIDI roster watcher"),
		m_client(client)
	{
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != B_MIDI_EVENT)
		{
			BLooper::MessageReceived(message);
			return;
		}

		// A burst of notifications (a device with many ports being plugged
		// in, our own connecting) needs only one refresh.
		if (!m_pending.exchange(true))
		{
			QMetaObject::invokeMethod(m_client, "refreshEndpoints", Qt::QueuedConnection);
		}
	}

	void refreshStarted() { m_pending = false; }

private:
	QObject* m_client;
	std::atomic<bool> m_pending{false};
};


QString endpointName(const char* name)
{
	return QString::fromUtf8(name ? name : "");
}


//! The first endpoint of a name keeps it, later ones get their ID appended.
void addEndpoint(QMap<QString, int32>& map, const QString& name, int32 id)
{
	QString key = name;
	if (map.contains(key)) { key = QString("%1 (%2)").arg(name).arg(id); }
	map.insert(key, id);
}

} // namespace



struct MidiHaiku::Impl : public EventSink
{
	struct Input
	{
		InEndpoint* endpoint = nullptr;
		MidiPortList ports;
		int32 connectedTo = -1; // ID of the producer it is connected to
	};

	struct Output
	{
		OutEndpoint* endpoint = nullptr;
		MidiPortList ports;
		int32 connectedTo = -1; // ID of the consumer it is connected to
	};

	explicit Impl(MidiHaiku* client) :
		client(client)
	{
		roster = BMidiRoster::MidiRoster();
		if (roster == nullptr) { return; }

		scan(producers, consumers);

		watcher = new RosterWatcher(client);
		watcher->Run();
		BMessenger messenger(watcher);
		BMidiRoster::StartWatching(&messenger);
	}

	~Impl() override
	{
		if (watcher != nullptr)
		{
			BMidiRoster::StopWatching();
			thread_id thread = watcher->Thread();
			watcher->Lock();
			watcher->Quit();
			status_t result;
			wait_for_thread(thread, &result);
		}

		// no lock: the endpoints are stopped one by one below
		for (auto it = inputs.begin(); it != inputs.end(); ++it) { destroy(it.value().endpoint); }
		for (auto it = outputs.begin(); it != outputs.end(); ++it) { destroy(it.value().endpoint); }
	}

	//! Takes the endpoint off the roster; its thread has ended once this returns.
	static void destroy(BMidiEndpoint* endpoint);

	//! Lists the endpoints other applications and drivers have registered.
	void scan(QMap<QString, int32>& producerMap, QMap<QString, int32>& consumerMap) const
	{
		int32 id = 0;
		while (BMidiProducer* producer = roster->NextProducer(&id))
		{
			if (!producer->IsLocal()) { addEndpoint(producerMap, endpointName(producer->Name()), producer->ID()); }
			producer->Release();
		}

		id = 0;
		while (BMidiConsumer* consumer = roster->NextConsumer(&id))
		{
			if (!consumer->IsLocal()) { addEndpoint(consumerMap, endpointName(consumer->Name()), consumer->ID()); }
			consumer->Release();
		}
	}

	//! Connects the endpoints to the remote ones they stand for, where that is not so yet.
	void connectEndpoints();

	void deliver(const QString& source, const void* endpoint, const MidiEvent& event) override
	{
		MidiPortList ports;
		{
			QMutexLocker locker(&mutex);
			const auto it = inputs.constFind(source);
			if (it == inputs.constEnd() || it->endpoint != endpoint) { return; }
			ports = it->ports;
		}

		// Delivered without the lock: handling an event may wait for the audio
		// engine, which must not be able to wait for us in turn.
		for (MidiPort* port : ports) { port->processInEvent(event); }
	}

	MidiHaiku* client;
	BMidiRoster* roster = nullptr;
	RosterWatcher* watcher = nullptr;

	mutable QMutex mutex; // guards everything below
	QMap<QString, int32> producers; // remote sources by name
	QMap<QString, int32> consumers; // remote sinks by name
	QMap<QString, Input> inputs; // by the name of the producer
	QMap<QString, Output> outputs; // by the name of the consumer
};



void MidiHaiku::Impl::destroy(BMidiEndpoint* endpoint)
{
	if (endpoint == nullptr) { return; }
	endpoint->Unregister();
	endpoint->Release();
}


void MidiHaiku::Impl::connectEndpoints()
{
	struct Job
	{
		BMidiEndpoint* endpoint;
		bool isInput;
		QString name;
		int32 wanted;
	};
	QVector<Job> jobs;

	{
		QMutexLocker locker(&mutex);
		for (auto it = inputs.begin(); it != inputs.end(); ++it)
		{
			const int32 wanted = producers.value(it.key(), -1);
			if (wanted == it->connectedTo) { continue; }
			it->endpoint->Acquire();
			jobs.append({it->endpoint, true, it.key(), wanted});
		}
		for (auto it = outputs.begin(); it != outputs.end(); ++it)
		{
			const int32 wanted = consumers.value(it.key(), -1);
			if (wanted == it->connectedTo) { continue; }
			it->endpoint->Acquire();
			jobs.append({it->endpoint, false, it.key(), wanted});
		}
	}

	// The roster is asked without the lock held; it answers through threads
	// that may be waiting for the lock themselves.
	for (const Job& job : jobs)
	{
		int32 connectedTo = -1;
		if (job.wanted != -1)
		{
			status_t result = B_ERROR;
			if (job.isInput)
			{
				if (BMidiProducer* producer = roster->FindProducer(job.wanted))
				{
					result = producer->Connect(static_cast<InEndpoint*>(job.endpoint));
					producer->Release();
				}
			}
			else if (BMidiConsumer* consumer = roster->FindConsumer(job.wanted))
			{
				result = static_cast<OutEndpoint*>(job.endpoint)->Connect(consumer);
				consumer->Release();
			}

			if (result == B_OK) { connectedTo = job.wanted; }
			else { qWarning("MidiHaiku: could not connect to \"%s\"", job.name.toUtf8().constData()); }
		}

		{
			QMutexLocker locker(&mutex);
			if (job.isInput)
			{
				const auto it = inputs.find(job.name);
				if (it != inputs.end() && it->endpoint == job.endpoint) { it->connectedTo = connectedTo; }
			}
			else
			{
				const auto it = outputs.find(job.name);
				if (it != outputs.end() && it->endpoint == job.endpoint) { it->connectedTo = connectedTo; }
			}
		}
		job.endpoint->Release();
	}
}



MidiHaiku::MidiHaiku() :
	MidiClient(),
	m_impl(new Impl(this))
{
}



MidiHaiku::~MidiHaiku() = default;



bool MidiHaiku::isRunning() const
{
	return m_impl->watcher != nullptr;
}



void MidiHaiku::processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort* port)
{
	// The endpoints are held while they are played to, as the user may
	// unsubscribe from the GUI thread at any time.
	QVector<OutEndpoint*> targets;
	{
		QMutexLocker locker(&m_impl->mutex);
		for (auto it = m_impl->outputs.constBegin(); it != m_impl->outputs.constEnd(); ++it)
		{
			if (it->connectedTo != -1 && it->ports.contains(const_cast<MidiPort*>(port)))
			{
				it->endpoint->Acquire();
				targets.append(it->endpoint);
			}
		}
	}
	if (targets.isEmpty()) { return; }

	const uchar channel = event.channel() & 0x0f;
	const auto data = [](int value) { return static_cast<uchar>(std::clamp(value, 0, 127)); };

	for (OutEndpoint* target : targets)
	{
		// performance time 0 is long past, so the consumer plays the event at once
		switch (event.type())
		{
		case MidiNoteOn:
			if (event.velocity() > 0) { target->SprayNoteOn(channel, data(event.key()), data(event.velocity()), 0); }
			else { target->SprayNoteOff(channel, data(event.key()), 0, 0); }
			break;
		case MidiNoteOff:
			target->SprayNoteOff(channel, data(event.key()), data(event.velocity()), 0);
			break;
		case MidiKeyPressure:
			target->SprayKeyPressure(channel, data(event.key()), data(event.velocity()), 0);
			break;
		case MidiControlChange:
			target->SprayControlChange(channel, data(event.controllerNumber()), data(event.controllerValue()), 0);
			break;
		case MidiProgramChange:
			target->SprayProgramChange(channel, data(event.program()), 0);
			break;
		case MidiChannelPressure:
			target->SprayChannelPressure(channel, data(event.channelPressure()), 0);
			break;
		case MidiPitchBend:
		{
			const int bend = std::clamp(static_cast<int>(event.param(0)), 0, 16383);
			target->SprayPitchBend(channel, bend & 0x7f, bend >> 7, 0);
			break;
		}
		default:
			qWarning("MidiHaiku: unhandled output event %d", static_cast<int>(event.type()));
			break;
		}
		target->Release();
	}
}



void MidiHaiku::applyPortMode(MidiPort* port)
{
	// no subscription may outlive a direction the port no longer has
	if (!port->isInputEnabled()) { removeFromInputs(port); }
	if (!port->isOutputEnabled()) { removeFromOutputs(port); }
}



void MidiHaiku::removePort(MidiPort* port)
{
	removeFromInputs(port);
	removeFromOutputs(port);
	MidiClient::removePort(port);
}



QStringList MidiHaiku::readablePorts() const
{
	QMutexLocker locker(&m_impl->mutex);
	return m_impl->producers.keys();
}



QStringList MidiHaiku::writablePorts() const
{
	QMutexLocker locker(&m_impl->mutex);
	return m_impl->consumers.keys();
}



QString MidiHaiku::sourcePortName(const MidiEvent& event) const
{
	if (event.sourcePort() == nullptr) { return MidiClient::sourcePortName(event); }

	QMutexLocker locker(&m_impl->mutex);
	for (auto it = m_impl->inputs.constBegin(); it != m_impl->inputs.constEnd(); ++it)
	{
		if (it->endpoint == event.sourcePort()) { return it.key(); }
	}
	return MidiClient::sourcePortName(event);
}



void MidiHaiku::subscribeReadablePort(MidiPort* port, const QString& dest, bool subscribe)
{
	if (!subscribe)
	{
		removeFromInputs(port, dest);
		return;
	}

	if (!port->isInputEnabled())
	{
		qWarning("port %s can't be subscribed!", port->displayName().toUtf8().constData());
		return;
	}

	InEndpoint* created = nullptr;
	{
		QMutexLocker locker(&m_impl->mutex);
		if (!m_impl->inputs.contains(dest)) { created = new InEndpoint(*m_impl, dest); }
	}
	if (created != nullptr)
	{
		if (created->Register() != B_OK)
		{
			qWarning("MidiHaiku: could not register an endpoint for \"%s\"", dest.toUtf8().constData());
			created->Release();
			return;
		}
		QMutexLocker locker(&m_impl->mutex);
		m_impl->inputs[dest].endpoint = created;
	}

	{
		QMutexLocker locker(&m_impl->mutex);
		MidiPortList& ports = m_impl->inputs[dest].ports;
		if (!ports.contains(port)) { ports.append(port); }
	}
	m_impl->connectEndpoints();
}



void MidiHaiku::subscribeWritablePort(MidiPort* port, const QString& dest, bool subscribe)
{
	if (!subscribe)
	{
		removeFromOutputs(port, dest);
		return;
	}

	if (!port->isOutputEnabled())
	{
		qWarning("port %s can't be subscribed!", port->displayName().toUtf8().constData());
		return;
	}

	OutEndpoint* created = nullptr;
	{
		QMutexLocker locker(&m_impl->mutex);
		if (!m_impl->outputs.contains(dest)) { created = new OutEndpoint(dest); }
	}
	if (created != nullptr)
	{
		if (created->Register() != B_OK)
		{
			qWarning("MidiHaiku: could not register an endpoint for \"%s\"", dest.toUtf8().constData());
			created->Release();
			return;
		}
		QMutexLocker locker(&m_impl->mutex);
		m_impl->outputs[dest].endpoint = created;
	}

	{
		QMutexLocker locker(&m_impl->mutex);
		MidiPortList& ports = m_impl->outputs[dest].ports;
		if (!ports.contains(port)) { ports.append(port); }
	}
	m_impl->connectEndpoints();
}



void MidiHaiku::removeFromInputs(MidiPort* port, const QString& dest)
{
	QVector<InEndpoint*> unused;
	{
		QMutexLocker locker(&m_impl->mutex);
		for (auto it = m_impl->inputs.begin(); it != m_impl->inputs.end();)
		{
			if (dest.isNull() || it.key() == dest) { it->ports.removeAll(port); }
			if (it->ports.isEmpty())
			{
				unused.append(it->endpoint);
				it = m_impl->inputs.erase(it);
			}
			else { ++it; }
		}
	}

	// stopped without the lock, as the endpoint's thread may be waiting for it
	for (InEndpoint* endpoint : unused) { Impl::destroy(endpoint); }
}



void MidiHaiku::removeFromOutputs(MidiPort* port, const QString& dest)
{
	QVector<OutEndpoint*> unused;
	{
		QMutexLocker locker(&m_impl->mutex);
		for (auto it = m_impl->outputs.begin(); it != m_impl->outputs.end();)
		{
			if (dest.isNull() || it.key() == dest) { it->ports.removeAll(port); }
			if (it->ports.isEmpty())
			{
				unused.append(it->endpoint);
				it = m_impl->outputs.erase(it);
			}
			else { ++it; }
		}
	}

	for (OutEndpoint* endpoint : unused) { Impl::destroy(endpoint); }
}



void MidiHaiku::refreshEndpoints()
{
	if (m_impl->watcher == nullptr) { return; }
	m_impl->watcher->refreshStarted();

	QMap<QString, int32> producers;
	QMap<QString, int32> consumers;
	m_impl->scan(producers, consumers);

	bool readableChanged = false;
	bool writableChanged = false;
	{
		QMutexLocker locker(&m_impl->mutex);
		readableChanged = producers.keys() != m_impl->producers.keys();
		writableChanged = consumers.keys() != m_impl->consumers.keys();
		m_impl->producers = producers;
		m_impl->consumers = consumers;
	}

	// reconnects what has come back, and notes what has gone
	m_impl->connectEndpoints();

	if (readableChanged) { emit readablePortsChanged(); }
	if (writableChanged) { emit writablePortsChanged(); }
}

} // namespace lmms

#endif // LMMS_BUILD_HAIKU
