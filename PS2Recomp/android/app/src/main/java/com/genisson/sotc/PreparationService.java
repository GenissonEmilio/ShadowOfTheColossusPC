package com.genisson.sotc;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/** Keeps an active launcher import or preparation visible to Android when the launcher is backgrounded. */
public final class PreparationService extends Service {
    private static final String CHANNEL = "game_preparation";
    private static final String CANCEL = "com.genisson.sotc.CANCEL_PREPARATION";
    private static final String SESSION_ID = "preparation_session";
    private static final int NOTIFICATION_ID = 1;

    // The launcher, service lifecycle, and notification updates all use the main thread.
    private static Session active;
    private static int nextSessionId;

    static final class Session {
        private final int id = ++nextSessionId;
        private final Runnable cancel;
        private String message;
        private int percent = -1;
        private PreparationService service;

        private Session(String message, Runnable cancel) {
            this.message = message;
            this.cancel = cancel;
        }
    }

    static Session begin(Context context, String message, Runnable cancel) {
        if (active != null) return null;
        Session session = new Session(message, cancel);
        active = session;
        try {
            context.startForegroundService(new Intent(context, PreparationService.class));
            return session;
        } catch (IllegalStateException | SecurityException error) {
            active = null;
            Log.w("SOTC", "Could not start the preparation service", error);
            return null;
        }
    }

    static void update(Session session, String message, int percent) {
        if (session == null || active != session
                || (session.percent == percent && session.message.equals(message))) return;
        session.message = message;
        session.percent = percent;
        if (session.service != null) session.service.postProgress(session);
    }

    static void finish(Session session) {
        if (session == null || active != session) return;
        active = null;
        PreparationService service = session.service;
        session.service = null;
        if (service != null) service.stopPreparation();
        // If start is pending, onStartCommand will satisfy the required
        // startForeground call and immediately stop without starting any work.
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        Session session = active;
        if (intent != null && CANCEL.equals(intent.getAction())) {
            if (session != null && session.id == intent.getIntExtra(SESSION_ID, 0)) cancel(session);
            else if (session == null) stopSelfResult(startId);
            return START_NOT_STICKY;
        }
        Context labels = LauncherLanguage.localized(this);
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                labels.getText(R.string.launcher_player_heading), NotificationManager.IMPORTANCE_LOW));
        try {
            Notification notification = notification(labels, session);
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
        } catch (IllegalStateException | SecurityException error) {
            Log.w("SOTC", "Could not promote the preparation service", error);
            active = null;
            stopSelf();
            return START_NOT_STICKY;
        }
        if (session == null) stopPreparation();
        else session.service = this;
        return START_NOT_STICKY;
    }

    private Notification notification(Context labels, Session session) {
        Intent launcher = new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent open = PendingIntent.getActivity(this, 0, launcher, PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_launcher_disc)
                .setContentTitle(labels.getText(R.string.launcher_player_heading))
                .setContentText(session == null ? labels.getText(R.string.launcher_prepare_starting) : session.message)
                .setContentIntent(open)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setOnlyAlertOnce(true)
                .setOngoing(true);
        if (session != null) {
            Intent cancel = new Intent(this, PreparationService.class).setAction(CANCEL).putExtra(SESSION_ID, session.id);
            PendingIntent action = PendingIntent.getService(this, session.id, cancel, PendingIntent.FLAG_IMMUTABLE);
            builder.addAction(new Notification.Action.Builder(null, labels.getText(R.string.launcher_prepare_cancel), action).build());
            builder.setProgress(100, Math.max(0, session.percent), session.percent < 0);
        }
        return builder.build();
    }

    private void postProgress(Session session) {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return;
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager.areNotificationsEnabled()) {
            manager.notify(NOTIFICATION_ID, notification(LauncherLanguage.localized(this), session));
        }
    }

    private void stopPreparation() {
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private static void cancel(Session session) {
        finish(session);
        session.cancel.run();
    }

    @Override public void onTimeout(int startId, int foregroundServiceType) {
        Session session = active;
        if (session != null && session.service == this) cancel(session);
        else stopPreparation();
    }

    @Override public void onDestroy() {
        Session session = active;
        if (session != null && session.service == this) {
            active = null;
            session.service = null;
            session.cancel.run();
        }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
