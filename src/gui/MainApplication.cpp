/*
 * MainApplication.cpp - Main QApplication handler
 *
 * Copyright (c) 2017-2017 Tres Finocchiaro <tres.finocchiaro/at/gmail.com>
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
#include "MainApplication.h"

#include <QDebug>
#include <QDragMoveEvent>
#include <QFileOpenEvent>

#include "Engine.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "Song.h"

namespace lmms::gui
{


MainApplication::MainApplication(int& argc, char** argv) :
	QApplication(argc, argv),
	m_queuedFile()
{
#if !defined(LMMS_BUILD_WIN32) && !defined(LMMS_BUILD_APPLE) && !defined(LMMS_BUILD_HAIKU)
	// Work around a bug of KXmlGui < 5.55
	// which breaks the recent files menu
	// https://bugs.kde.org/show_bug.cgi?id=337491
	for (auto child : children())
	{
		if (child->inherits("KCheckAcceleratorsInitializer"))
		{
			delete child;
		}
	}
#endif

#if defined(LMMS_BUILD_WIN32)
	installNativeEventFilter(this);
#endif
}

#ifdef LMMS_BUILD_HAIKU
// Qt's drag and drop within an application, which is what Haiku has, takes a
// widget's acceptance of a drag when it enters as the answer for the moves that
// follow it, unless the widget says otherwise. A move that comes after the
// cursor has stopped, as the last one before the button is released does, was
// not taken that way here, which made Qt cancel the drop. Widgets of LMMS decide
// when the drag enters, so a move that nobody has handled stays accepted.
bool MainApplication::notify(QObject* receiver, QEvent* event)
{
	const auto type = event->type();
	const auto result = QApplication::notify(receiver, event);

	// the window gets each drag event too, and passes it on to a widget
	if (!receiver->isWidgetType()) { return result; }

	switch (type)
	{
	case QEvent::DragEnter:
		m_dragTarget = event->isAccepted() ? receiver : nullptr;
		break;
	case QEvent::DragMove:
		if (!event->isAccepted() && receiver == m_dragTarget)
		{
			static_cast<QDragMoveEvent*>(event)->acceptProposedAction();
		}
		break;
	case QEvent::DragLeave:
	case QEvent::Drop:
		m_dragTarget = nullptr;
		break;
	default:
		break;
	}

	return result;
}
#endif

bool MainApplication::event(QEvent* event)
{
	switch(event->type())
	{
		case QEvent::FileOpen:
		{
			auto fileEvent = static_cast<QFileOpenEvent*>(event);
			// Handle the project file
			m_queuedFile = fileEvent->file();
			if(Engine::getSong())
			{
				if(getGUI()->mainWindow()->mayChangeProject(true))
				{
					qDebug() << "Loading file " << m_queuedFile;
					Engine::getSong()->loadProject(m_queuedFile);
				}
			}
			else
			{
				qDebug() << "Queuing file " << m_queuedFile;
			}
			return true;
		}
		default:
			return QApplication::event(event);
	}
}

#ifdef LMMS_BUILD_WIN32
// Helper function for nativeEventFilter
bool MainApplication::win32EventFilter(MSG* msg, FilterResult* result)
{
	switch(msg->message)
	{
		case WM_STYLECHANGING:
			if (msg->wParam == static_cast<WPARAM>(GWL_EXSTYLE))
			{
				// Prevent plugins making the main window transparent
				STYLESTRUCT * style = reinterpret_cast<STYLESTRUCT *>(msg->lParam);
				if(!(style->styleOld & WS_EX_LAYERED))
				{
					style->styleNew &= ~WS_EX_LAYERED;
				}
				*result = 0;
				return true;
			}
			return false;
		default:
			return false;
	}
}

bool MainApplication::nativeEventFilter(const QByteArray& eventType,
					void* message, FilterResult* result)
{
	if(eventType == "windows_generic_MSG")
	{
		return win32EventFilter(static_cast<MSG*>(message), result);
	}
	return false;
}
#endif // LMMS_BUILD_WIN32


} // namespace lmms::gui
