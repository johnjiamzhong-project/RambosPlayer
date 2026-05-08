#include <QtTest>
#include "framequeue.h"
#include <QThread>
#include <atomic>

class TstFrameQueue : public QObject {
    Q_OBJECT
private slots:
    void pushPop_singleThread() {
        FrameQueue<int> q(10);
        q.push(42);
        int val = -1;
        QVERIFY(q.tryPop(val, 100));
        QCOMPARE(val, 42);
    }

    void tryPop_returnsFlase_whenEmpty() {
        FrameQueue<int> q(10);
        int val = -1;
        QVERIFY(!q.tryPop(val, 50));
    }

    void blocksAt_maxSize() {
        FrameQueue<int> q(2);
        q.push(1);
        q.push(2);
        std::atomic<bool> pushed{false};
        QThread* t = QThread::create([&]{
            q.push(3);
            pushed = true;
        });
        t->start();
        QThread::msleep(80);
        QVERIFY(!pushed);
        int v;
        q.tryPop(v, 50);
        QThread::msleep(50);
        QVERIFY(pushed);
        t->wait();
        delete t;
    }

    void abort_unblocks_pop() {
        FrameQueue<int> q(10);
        std::atomic<bool> done{false};
        QThread* t = QThread::create([&]{
            int v;
            q.tryPop(v, 5000);
            done = true;
        });
        t->start();
        QThread::msleep(30);
        q.abort();
        t->wait(500);
        QVERIFY(done);
        delete t;
    }

    void size_and_clear() {
        FrameQueue<int> q(10);
        q.push(1); q.push(2); q.push(3);
        QCOMPARE(q.size(), 3);
        q.clear();
        QCOMPARE(q.size(), 0);
    }
};

QTEST_MAIN(TstFrameQueue)
#include "tst_framequeue.moc"
