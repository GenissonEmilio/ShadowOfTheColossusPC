package com.genisson.sotc;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class PreparationWorkersTest {
    public static void main(String[] args) throws Exception {
        verifyProcessCleanup();
        verifyWorkerCleanup();
        System.out.println("Preparation cancellation tests passed.");
    }

    private static void verifyProcessCleanup() throws Exception {
        DeferredExit process = new DeferredExit();
        CountDownLatch finished = new CountDownLatch(1);
        AtomicBoolean preservedInterrupt = new AtomicBoolean();
        Thread cleanup = new Thread(() -> {
            PreparationWorkers.stop(process);
            preservedInterrupt.set(Thread.currentThread().isInterrupted());
            finished.countDown();
        });
        cleanup.start();
        try {
            await(process.signalled, "process was not killed");
            await(process.waiting, "cleanup did not wait for process exit");
            cleanup.interrupt();
            check(!finished.await(80, TimeUnit.MILLISECONDS), "cleanup returned while process was still alive");
        } finally {
            process.exited.countDown();
            cleanup.join(5000);
        }
        check(!cleanup.isAlive() && preservedInterrupt.get(), "process cleanup lost interrupt status");
    }

    private static void verifyWorkerCleanup() throws Exception {
        var pool = Executors.newSingleThreadExecutor();
        CountDownLatch running = new CountDownLatch(1);
        CountDownLatch interrupted = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch finished = new CountDownLatch(1);
        AtomicBoolean preservedInterrupt = new AtomicBoolean();
        pool.submit(() -> {
            running.countDown();
            while (true) {
                try {
                    release.await();
                    return;
                } catch (InterruptedException error) {
                    interrupted.countDown();
                }
            }
        });
        Thread cleanup = new Thread(() -> {
            PreparationWorkers.stop(pool);
            preservedInterrupt.set(Thread.currentThread().isInterrupted());
            finished.countDown();
        });
        try {
            await(running, "test worker did not start");
            cleanup.start();
            await(interrupted, "worker did not receive cancellation");
            cleanup.interrupt();
            check(!finished.await(80, TimeUnit.MILLISECONDS), "cleanup returned while a worker was still running");
        } finally {
            release.countDown();
            pool.shutdownNow();
            cleanup.join(5000);
        }
        check(!cleanup.isAlive() && pool.isTerminated() && preservedInterrupt.get(),
                "worker cleanup lost interrupt status or left a worker alive");
    }

    private static void await(CountDownLatch latch, String message) throws InterruptedException {
        check(latch.await(5, TimeUnit.SECONDS), message);
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static final class DeferredExit extends Process {
        final CountDownLatch signalled = new CountDownLatch(1);
        final CountDownLatch waiting = new CountDownLatch(1);
        final CountDownLatch exited = new CountDownLatch(1);

        @Override public OutputStream getOutputStream() { return new ByteArrayOutputStream(); }
        @Override public InputStream getInputStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public boolean isAlive() { return exited.getCount() != 0; }
        @Override public int exitValue() {
            if (isAlive()) throw new IllegalThreadStateException();
            return 137;
        }
        @Override public int waitFor() throws InterruptedException {
            waiting.countDown();
            exited.await();
            return exitValue();
        }
        @Override public void destroy() { signalled.countDown(); }
        @Override public Process destroyForcibly() { destroy(); return this; }
    }
}
