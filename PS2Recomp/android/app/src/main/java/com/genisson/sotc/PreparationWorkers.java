package com.genisson.sotc;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;

/** Cancels compiler work and waits for ownership of its scratch files to end. */
final class PreparationWorkers {
    static void stop(Process process) {
        if (process.isAlive()) process.destroyForcibly();
        boolean interrupted = false;
        try {
            while (true) {
                try {
                    process.waitFor();
                    return;
                } catch (InterruptedException error) {
                    interrupted = true;
                }
            }
        } finally {
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    static void stop(ExecutorService workers) {
        workers.shutdownNow();
        boolean interrupted = false;
        try {
            while (!workers.isTerminated()) {
                try {
                    workers.awaitTermination(Long.MAX_VALUE, TimeUnit.NANOSECONDS);
                } catch (InterruptedException error) {
                    interrupted = true;
                }
            }
        } finally {
            if (interrupted) Thread.currentThread().interrupt();
        }
    }
}
