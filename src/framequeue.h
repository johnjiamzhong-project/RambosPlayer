#pragma once
#include <queue>
#include <QMutex>
#include <QWaitCondition>

// 线程安全的有界阻塞队列，用于生产者-消费者管道。
// 队列满时 push 阻塞，队列空时 tryPop 按超时等待；
// abort() 立即唤醒所有等待线程，reset() 清空并恢复正常状态。
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

    // 非阻塞 push，队列满时立即返回 false，避免生产者因队列满而阻塞。
    // 用于视频包队列：丢视频帧可接受，但不能因视频队列满而阻塞音频包生产。
    bool tryPush(T item) {
        QMutexLocker lk(&mutex_);
        if (aborted_) return false;
        if ((int)q_.size() >= maxSize_) return false;
        q_.push(std::move(item));
        notEmpty_.wakeOne();
        return true;
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
