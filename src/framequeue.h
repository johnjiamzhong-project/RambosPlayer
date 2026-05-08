#pragma once
#include <queue>
#include <QMutex>
#include <QWaitCondition>

template<typename T>
class FrameQueue {
public:
    explicit FrameQueue(int maxSize) : maxSize_(maxSize) {}

    void push(T item) {
        QMutexLocker lk(&mutex_);
        while (!aborted_ && (int)q_.size() >= maxSize_)
            notFull_.wait(&mutex_);
        if (aborted_) return;
        q_.push(std::move(item));
        notEmpty_.wakeOne();
    }

    bool tryPop(T& out, int timeoutMs) {
        QMutexLocker lk(&mutex_);
        if (q_.empty() && !aborted_)
            notEmpty_.wait(&mutex_, timeoutMs);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        notFull_.wakeOne();
        return true;
    }

    void clear() {
        QMutexLocker lk(&mutex_);
        while (!q_.empty()) q_.pop();
        notFull_.wakeAll();
    }

    void abort() {
        QMutexLocker lk(&mutex_);
        aborted_ = true;
        notEmpty_.wakeAll();
        notFull_.wakeAll();
    }

    void reset() {
        QMutexLocker lk(&mutex_);
        aborted_ = false;
        while (!q_.empty()) q_.pop();
    }

    int size() const {
        QMutexLocker lk(&mutex_);
        return (int)q_.size();
    }

private:
    mutable QMutex mutex_;
    QWaitCondition notEmpty_;
    QWaitCondition notFull_;
    std::queue<T> q_;
    int maxSize_;
    bool aborted_ = false;
};
