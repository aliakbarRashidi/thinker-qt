//
// ThinkerManager.cpp
// This file is part of Thinker-Qt
// Copyright (C) 2010 HostileFork.com
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

#include <QThreadPool>
#include <QMutexLocker>

#include "thinkerqt/thinkerrunner.h"
#include "thinkerqt/thinkermanager.h"


//
// ThinkerManager
//

#if not THINKERQT_EXPLICIT_MANAGER
ThinkerManager & ThinkerManager::getGlobalManager() {
    // This may depend on C++11 guarantees if you call this from more than one thread
    // http://stackoverflow.com/questions/9608257/
    static ThinkerManager globalInstance;
    return globalInstance;
}
#endif


ThinkerManager::ThinkerManager () :
	QObject (),
	anyThinkerWrittenThrottler (400)
{
	hopefullyCurrentThreadIsManager(HERE);

	connect(&anyThinkerWrittenThrottler, SIGNAL(throttled()), this, SIGNAL(anyThinkerWritten()), Qt::DirectConnection);
	connect(this, SIGNAL(pushToThreadMayBeNeeded()),
		this, SLOT(doThreadPushesIfNecessary()), Qt::QueuedConnection);

}

bool ThinkerManager::hopefullyThreadIsManager(const QThread & thread, codeplace const & cp)
{
	return hopefully(&thread == this->thread(), cp);
}

bool ThinkerManager::hopefullyCurrentThreadIsManager(codeplace const & cp)
{
	return hopefullyThreadIsManager(*QThread::currentThread(), cp);
}

bool ThinkerManager::hopefullyThreadIsNotManager(const QThread & thread, codeplace const & cp)
{
	return hopefully(&thread != this->thread(), cp);
}

bool ThinkerManager::hopefullyCurrentThreadIsNotManager(codeplace const & cp)
{
	return hopefullyThreadIsNotManager(*QThread::currentThread(), cp);
}

bool ThinkerManager::hopefullyThreadIsThinker(const QThread & thread, codeplace const & cp)
{
    return hopefully(maybeGetRunnerForThread(thread) != nullptr, cp);
}

bool ThinkerManager::hopefullyCurrentThreadIsThinker(codeplace const & cp)
{
	return hopefullyThreadIsThinker(*QThread::currentThread(), cp);
}

void ThinkerManager::createRunnerForThinker(shared_ptr<ThinkerBase> holder, codeplace const & cp)
{
	hopefullyCurrentThreadIsManager(cp);
/*	hopefully(not holder.isNull(), cp); */

    shared_ptr<ThinkerRunner> runner (new ThinkerRunner (holder));
	ThinkerRunnerProxy* runnerProxy (new ThinkerRunnerProxy (runner));

	// this may look like a bad idea because we are dynamically allocating and not hanging onto
	// the result so we can free it.  but it's okay because ThinkerRunners maintain a global table
	// which they insert themselves into on construction and delete themselves from during
	// destruction.  Also, when the thread emits the finished signal, we clean it up using deleteLater
	// (it is freed by the event loop when all events have been processed)
	runnerProxy->setAutoDelete(true);

	// QtConcurrent defines one global thread pool instance but maybe I'll let you specify your
	// own, not sure if that's useful.  They make a lot of global assumptions, perhaps I should
	// just piggy back on them.

	// Queue this runnable thing to the thread pool.  It may take a while before
	// a thread gets allocated to it.
	static_cast<void>(QThreadPool::globalInstance()->start(runnerProxy));
}

void ThinkerManager::ensureThinkersPaused(codeplace const & cp)
{
	hopefullyCurrentThreadIsManager(HERE);

	QMutexLocker locker (&mapsMutex);
	// we have to make a copy of the map
    QMap<const ThinkerBase *, shared_ptr<ThinkerRunner> > mapCopy (thinkerMap);
	locker.unlock();

    QMapIterator<const ThinkerBase *, shared_ptr<ThinkerRunner> > i (mapCopy);

	// First pass: request all thinkers to pause (accept it if they are aborting, as they
	// may be freed by the ThinkerPresent but not yet returned).
	while (i.hasNext()) {
		i.next();
        shared_ptr<ThinkerRunner> runner (i.value());
		runner->requestPauseButCanceledIsOkay(cp);
	}

	i.toFront();

	// Second pass: wait for all the thinkers to actually get their code off the stack.
	while (i.hasNext()) {
		i.next();
        shared_ptr<ThinkerRunner> runner (i.value());
		runner->waitForPauseButCanceledIsOkay();
	}
}

void ThinkerManager::ensureThinkersResumed(codeplace const & cp)
{
	hopefullyCurrentThreadIsManager(HERE);

	QMutexLocker locker (&mapsMutex);

	// any thinkers that have not been aborted can be resumed
    QMapIterator<const ThinkerBase *, shared_ptr<ThinkerRunner>> i = thinkerMap;
	while (i.hasNext()) {
		i.next();
        shared_ptr<ThinkerRunner> runner (i.value());
		if (runner->isPaused())
			runner->requestResumeButCanceledIsOkay(cp);
	}
}

shared_ptr<ThinkerRunner> ThinkerManager::maybeGetRunnerForThread(const QThread & thread)
{
    QMutexLocker locker (&mapsMutex);
    shared_ptr<ThinkerRunner> result (threadMap.value(&thread, nullptr));
	return result;
}

shared_ptr<ThinkerRunner> ThinkerManager::maybeGetRunnerForThinker(const ThinkerBase & thinker)
{
	QMutexLocker locker (&mapsMutex);
    shared_ptr<ThinkerRunner> result (thinkerMap.value(&thinker, nullptr));
    if (result == nullptr) {
		hopefully(
			(thinker.state == ThinkerBase::ThinkerCanceled)
			or (thinker.state == ThinkerBase::ThinkerFinished),
			HERE
		);
    }
	return result;
}

const ThinkerBase * ThinkerManager::getThinkerForThreadMaybeNull(const QThread & thread)
{
    shared_ptr<ThinkerRunner> runner (maybeGetRunnerForThread(thread));
    if (runner == nullptr) {
        return nullptr;
	}
	return &(runner->getThinker());
}

void ThinkerManager::requestAndWaitForCancelButAlreadyCanceledIsOkay(ThinkerBase & thinker)
{
    shared_ptr<ThinkerRunner> runner (maybeGetRunnerForThinker(thinker));
    if (runner == nullptr) {
		thinker.state = ThinkerBase::ThinkerCanceled;
	} else {
		// thread should be paused or finished... or possibly aborted
		runner->requestCancelButAlreadyCanceledIsOkay(HERE);
		runner->waitForFinished(HERE);
	}
	hopefully(thinker.state == ThinkerBase::ThinkerCanceled, HERE);
}


void ThinkerManager::ensureThinkerFinished(ThinkerBase & thinker)
{
	hopefullyCurrentThreadIsManager(HERE);

    shared_ptr<ThinkerRunner> runner = maybeGetRunnerForThinker(thinker);
    if (runner != nullptr) {
    	// can't finish if it's aborted or invalid!
		hopefully(not runner->isCanceled(), HERE); 

		// we need to watch the state changes and ensure that
		// it completes... note user cancellation would mean that it
		// would not so we have to allow for that case!
        if (runner->isPaused()) {
            runner->requestResume(HERE);
            runner->waitForResume(HERE);
		}

        runner->waitForFinished(HERE);
        hopefully(runner->isFinished(), HERE);
        thinker.state = ThinkerBase::ThinkerFinished;
	}

	hopefully(thinker.state == ThinkerBase::ThinkerFinished, HERE);
}


void ThinkerManager::unlockThinker(ThinkerBase & thinker)
{
	// do throttled emit to all the ThinkerPresentWatchers
	thinker.watchersLock.lockForRead();
	QSetIterator<ThinkerPresentWatcherBase*> i (thinker.watchers);
	while (i.hasNext()) {
		i.next()->notificationThrottler->emitThrottled();
	}
	thinker.watchersLock.unlock();

	// there is a notification throttler for all thinkers.  Review: should it
	// be possible to have a separate notification for groups?
	anyThinkerWrittenThrottler.emitThrottled();
}


void ThinkerManager::addToThinkerMap(shared_ptr<ThinkerRunner> runner)
{
	// We use a mutex to guard the addition and removal of Runners to the maps
	// If a Runner exists, then we look to its state information for
	// cancellation--not the Thinker.

	QMutexLocker locker (&mapsMutex);
	ThinkerBase & thinker (runner->getThinker());
	hopefully(not thinkerMap.contains(&thinker), HERE);
	thinkerMap.insert(&thinker, runner);
}


void ThinkerManager::removeFromThinkerMap(
	shared_ptr<ThinkerRunner> runner,
	bool wasCanceled
) {
	QMutexLocker locker (&mapsMutex);

	ThinkerBase & thinker (runner->getThinker());
	hopefully(thinkerMap.remove(&thinker) == 1, HERE);

	hopefully(thinker.state == ThinkerBase::ThinkerOwnedByRunner, HERE);
	thinker.state = wasCanceled
		? ThinkerBase::ThinkerCanceled
		: ThinkerBase::ThinkerFinished;
}


void ThinkerManager::addToThreadMap(
	shared_ptr<ThinkerRunner> runner,
	QThread & thread
) {
	QMutexLocker locker (&mapsMutex);
	hopefully(not threadMap.contains(&thread), HERE);
	threadMap.insert(&thread, runner);
}


void ThinkerManager::removeFromThreadMap(
	shared_ptr<ThinkerRunner> runner,
	QThread & thread
) {
	Q_UNUSED(runner);
	QMutexLocker locker (&mapsMutex);
	hopefully(threadMap.remove(&thread) == 1, HERE);
}


void ThinkerManager::waitForPushToThread(ThinkerRunner* runner)
{
	hopefullyCurrentThreadIsNotManager(HERE);

	QMutexLocker locker (&pushThreadMutex);
	runnerSetToPush.insert(runner);
	threadsNeedPushing.wakeOne();
	emit pushToThreadMayBeNeeded();
	threadsWerePushed.wait(&pushThreadMutex);
}


void ThinkerManager::processThreadPushesUntil(ThinkerRunner* runner)
{
	hopefullyCurrentThreadIsManager(HERE);

	QMutexLocker locker (&pushThreadMutex);
	forever {
		bool found = false;
		foreach (ThinkerRunner* runnerToPush, runnerSetToPush) {
     			runnerToPush->doThreadPushIfNecessary();
            if (runner and (runnerToPush == runner))
				found = true;
		}
		runnerSetToPush.clear();
		threadsWerePushed.wakeAll();
        if (found or not runner)
			return;
		threadsNeedPushing.wait(&pushThreadMutex);
	}
}


void ThinkerManager::doThreadPushesIfNecessary()
{
	processThreadPushesUntil(NULL);
}


ThinkerManager::~ThinkerManager()
{
	hopefullyCurrentThreadIsManager(HERE);

	// We catch you with an assertion if you do not make sure all your
	// Presents have been either canceled or completed
	bool anyRunners (false);

	if (true) {
		QMutexLocker locker (&mapsMutex);
        QMapIterator<const ThinkerBase *, shared_ptr<ThinkerRunner>> i (thinkerMap);
		while (i.hasNext()) {
			i.next();
            shared_ptr<ThinkerRunner> runner (i.value());
			hopefully(runner->isCanceled() or runner->isFinished(), HERE);
			anyRunners = true;
		}
	}

	if (anyRunners)
		QThreadPool::globalInstance()->waitForDone();
}
