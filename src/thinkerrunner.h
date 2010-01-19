//
// ThinkerRunner.h
// This file is part of Thinker-Qt
// Copyright (C) 2009 HostileFork.com
//
// Thinker-Qt is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Thinker-Qt is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Thinker-Qt.  If not, see <http://www.gnu.org/licenses/>.
//
// See http://hostilefork.com/thinker-qt/ for more information on this project
//

#ifndef THINKERQT__THINKERRUNNER_H
#define THINKERQT__THINKERRUNNER_H

#include <QWaitCondition>
#include <QMutex>
#include <QSemaphore>
#include <QRunnable>
#include <QEventLoop>

#include "thinkerqt/thinker.h"

class ThinkerRunner;
class ThinkerManager;

// pretty much every thread object needs a member who was
// created in the thread's run() method, and thus dispatches
// messages within the thread's context
class ThinkerRunnerHelper : public QObject
{
	Q_OBJECT

private:
	ThinkerRunner* runner;

public:
	ThinkerRunnerHelper(ThinkerRunner* runner);
	~ThinkerRunnerHelper();

public slots:
	void markFinished();
	void queuedQuit();
};

class ThinkerRunner : public QEventLoop, public QRunnable
{
	Q_OBJECT

private:
	enum State {
		RunnerInitializing, // => Thinking
		RunnerThinking, // => Pausing, Canceling, Finished
		RunnerPausing, // => Paused
		RunnerPaused, // => Canceled, Resuming
		RunnerCanceling, // => Canceled
		RunnerCanceled, // terminal
		RunnerResuming, // => Thinking
		RunnerFinished // => Canceled
	};

private:
#ifndef Q_NO_EXCEPTIONS
	class StopException {
	};
#endif

// http://www.learncpp.com/cpp-tutorial/93-overloading-the-io-operators/
friend QTextStream& operator << (QTextStream& o, const State& state);

private:
	tracked< State > state;
	// it's for communication between one manager and one thinker so use wakeOne()
	mutable QWaitCondition stateChangeSignal;
	mutable QMutex signalMutex;
	ThinkerHolder< ThinkerObject > holder;

friend class ThinkerRunnerHelper;

public:
	ThinkerManager& getManager() const;
	ThinkerObject& getThinker();
	const ThinkerObject& getThinker() const;

	// It used to be that Thinkers (QObjects) were created on the Manager thread and then pushed
	// to a thread of their own during the Run.  Since Run now queues, that push is deferred.  We
	// only know which thread the ThreadPool will put a Thinker onto when ThreadRunner::run()
	// happens, so we make a moveThinkerToThread request from that
signals:
	void moveThinkerToThread(QThread* thread, QSemaphore* numThreadsMoved);

public slots:
	void onMoveThinkerToThread(QThread* thread, QSemaphore* numThreadsMoved);

public:
	bool hopefullyCurrentThreadIsPooled(const codeplace& cp) const {
		// TODO should do a stronger check w/the manager to make sure that we not only
		// are a thinker thread but the thinker thread currently running in the thread
		// pool
		//return getManager().hopefullyCurrentThreadIsThinker(cp);
		return true;
	}
	void setPriority(QThread::Priority priority) {
		// stopgap routine.  For the moment let's not touch priorities.
	}

public:
	ThinkerRunner (ThinkerHolder< ThinkerObject > holder);

signals:
	void breakEventLoop();
	void startThinking();
	void resumeThinking();
	void attachingToThinker();
	void detachingFromThinker();

private:
	void requestPauseCore(bool isCanceledOkay, const codeplace& cp);
	void waitForPauseCore(bool isCanceledOkay);
	void requestCancelCore(bool isAlreadyCanceledOkay, const codeplace& cp);
	void requestResumeCore(bool isCanceledOkay, const codeplace& cp);

public slots:
	void requestPause(const codeplace& cp) {
		requestPauseCore(false, cp);
	}
	void waitForPause() {
		waitForPauseCore(false);
	}

	void requestPauseButCanceledIsOkay(const codeplace& cp) {
		requestPauseCore(true, cp);
	}
	void waitForPauseButCanceledIsOkay() {
		waitForPauseCore(true);
	}

	void requestCancel(const codeplace& cp) {
		requestCancelCore(false, cp);
	}
	void requestCancelButAlreadyCanceledIsOkay(const codeplace& cp) {
		requestCancelCore(true, cp);
	}

	void waitForCancel(); // no variant because we can't distinguish between an abort we asked for and

	void requestResume(const codeplace& cp) {
		requestResumeCore(false, cp);
	}
	void requestResumeButCanceledIsOkay(const codeplace& cp) {
		requestResumeCore(true, cp);
	}
	void waitForResume(const codeplace& cp);

	void requestFinishAndWaitForFinish(const codeplace& cp);

public:
	bool isFinished() const;
	bool isCanceled() const;
	bool isPaused() const;
	bool wasPauseRequested(unsigned long time = 0) const;
#ifndef Q_NO_EXCEPTIONS
	void pollForStopException(unsigned long time = 0) const;
#endif

protected:
	void run();

signals:
	void finished(ThinkerObject* thinker, bool canceled); // used to inherit from Thread

public:
	virtual ~ThinkerRunner();
};

#endif
