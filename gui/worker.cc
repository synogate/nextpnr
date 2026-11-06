/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <micko@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "worker.h"
#include <fstream>
#include "design_utils.h"
#include "log.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

struct WorkerInterruptionRequested
{
};

Worker::Worker(TaskManager *parent) : ctx(nullptr)
{
    log_write_function = [this, parent](std::string text) {
        Q_EMIT log(text);
        if (parent->shouldTerminate()) {
            parent->clearTerminate();
            throw WorkerInterruptionRequested();
        }
        if (parent->isPaused()) {
            Q_EMIT taskPaused();
        }
        while (parent->isPaused()) {
            if (parent->shouldTerminate()) {
                parent->clearTerminate();
                throw WorkerInterruptionRequested();
            }
            QThread::sleep(1);
        }
    };
}

void Worker::newContext(Context *ctx_) { ctx = ctx_; }

void Worker::pack()
{
    Q_EMIT taskStarted();
    try {
        bool res = ctx->pack();
        print_utilisation(ctx);
        Q_EMIT pack_finished(res);
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    } catch (log_execution_error_exception) {
        // The pass failed (log_error): the step ends, and the error goes on to the GUI
        Q_EMIT pack_finished(false);
        throw;
    } catch (const std::exception &e) {
        log_nonfatal_error("Internal error: %s\n", e.what());
        Q_EMIT pack_finished(false);
    }
}

void Worker::place(bool timing_driven)
{
    Q_EMIT taskStarted();
    // Placing again (after cells were unplaced by hand, maybe in a routed design): only the unplaced cells are
    // placed, the others keep their bels for this pass (else the refinement would move every cell that is not
    // locked and break the routing there)
    std::vector<std::pair<IdString, PlaceStrength>> held;
    if (ctx->settings.count(ctx->id("place"))) {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->bel != BelId() && ci->belStrength < STRENGTH_LOCKED) {
                held.emplace_back(ci->name, ci->belStrength);
                ci->belStrength = STRENGTH_LOCKED;
            }
        }
        if (!held.empty())
            log_info("Placing the unplaced cells only: %d placed cells keep their bels.\n", int(held.size()));
    }
    auto release = [&] {
        for (auto &h : held) {
            auto found = ctx->cells.find(h.first);
            if (found != ctx->cells.end() && found->second->bel != BelId() &&
                found->second->belStrength == STRENGTH_LOCKED)
                found->second->belStrength = h.second;
        }
    };
    try {
        ctx->settings[ctx->id("timing_driven")] = std::to_string(timing_driven);
        bool result = ctx->place();
        release();
        Q_EMIT place_finished(result);
    } catch (WorkerInterruptionRequested) {
        release();
        Q_EMIT taskCanceled();
    } catch (log_execution_error_exception) {
        // The pass failed (log_error): the step ends, and the error goes on to the GUI
        release();
        Q_EMIT place_finished(false);
        throw;
    } catch (const std::exception &e) {
        release();
        log_nonfatal_error("Internal error: %s\n", e.what());
        Q_EMIT place_finished(false);
    }
}

void Worker::route()
{
    Q_EMIT taskStarted();
    try {
        Q_EMIT route_finished(ctx->route());
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    } catch (log_execution_error_exception) {
        // The pass failed (log_error): the step ends, and the error goes on to the GUI
        Q_EMIT route_finished(false);
        throw;
    } catch (const std::exception &e) {
        log_nonfatal_error("Internal error: %s\n", e.what());
        Q_EMIT route_finished(false);
    }
}

void Worker::run(std::function<bool()> step)
{
    Q_EMIT taskStarted();
    try {
        Q_EMIT run_finished(step());
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    } catch (log_execution_error_exception) {
        // The pass failed (log_error): the step ends, and the error goes on to the GUI
        Q_EMIT run_finished(false);
        throw;
    } catch (const std::exception &e) {
        log_nonfatal_error("Internal error: %s\n", e.what());
        Q_EMIT run_finished(false);
    }
}

TaskManager::TaskManager() : toTerminate(false), toPause(false)
{
    qRegisterMetaType<std::function<bool()>>("std::function<bool()>");

    Worker *worker = new Worker(this);
    worker->moveToThread(&workerThread);

    connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);

    connect(this, &TaskManager::pack, worker, &Worker::pack);
    connect(this, &TaskManager::place, worker, &Worker::place);
    connect(this, &TaskManager::route, worker, &Worker::route);
    connect(this, &TaskManager::run, worker, &Worker::run);

    connect(this, &TaskManager::contextChanged, worker, &Worker::newContext);

    connect(worker, &Worker::log, this, &TaskManager::info);
    connect(worker, &Worker::pack_finished, this, &TaskManager::pack_finished);
    connect(worker, &Worker::place_finished, this, &TaskManager::place_finished);
    connect(worker, &Worker::route_finished, this, &TaskManager::route_finished);
    connect(worker, &Worker::run_finished, this, &TaskManager::run_finished);

    connect(worker, &Worker::taskCanceled, this, &TaskManager::taskCanceled);
    connect(worker, &Worker::taskStarted, this, &TaskManager::taskStarted);
    connect(worker, &Worker::taskPaused, this, &TaskManager::taskPaused);

    workerThread.start();
}

TaskManager::~TaskManager()
{
    log_write_function = nullptr;
    if (workerThread.isRunning())
        terminate_thread();
    workerThread.quit();
    workerThread.wait();
}

void TaskManager::info(const std::string &result) { Q_EMIT log(result); }

void TaskManager::terminate_thread()
{
    QMutexLocker locker(&mutex);
    toPause = false;
    toTerminate = true;
}

bool TaskManager::shouldTerminate()
{
    QMutexLocker locker(&mutex);
    return toTerminate;
}

void TaskManager::clearTerminate()
{
    QMutexLocker locker(&mutex);
    toTerminate = false;
}

void TaskManager::pause_thread()
{
    QMutexLocker locker(&mutex);
    toPause = true;
}

void TaskManager::continue_thread()
{
    QMutexLocker locker(&mutex);
    toPause = false;
    Q_EMIT taskStarted();
}

bool TaskManager::isPaused()
{
    QMutexLocker locker(&mutex);
    return toPause;
}

NEXTPNR_NAMESPACE_END
