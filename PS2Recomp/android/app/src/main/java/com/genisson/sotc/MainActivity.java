package com.genisson.sotc;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.StatFs;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.UUID;
import java.util.concurrent.FutureTask;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;
import java.util.function.IntConsumer;

public final class MainActivity extends Activity {
    private static final int IMPORT_DISC = 100;
    private static final int IMPORT_PLAYER = 101;
    private static final String BOOT_SHA256 = BuildConfig.PORTABLE ? PlayerPackage.BOOT_SHA256 : BuildConfig.BOOT_SHA256;
    private static final ThreadPoolExecutor DISK_WORKER = new ThreadPoolExecutor(
            1, 1, 30, TimeUnit.SECONDS, new LinkedBlockingQueue<>());
    static { DISK_WORKER.allowCoreThreadTimeOut(true); }

    private SharedPreferences preferences;
    private DiscRepository discs;
    private PlayerPackage player;
    private Context localized;
    private boolean canPrepare;
    private boolean playerInstalled;
    private final ReentrantLock discCommit = new ReentrantLock();
    private DiscRepository.Verification verifiedDisc = DiscRepository.Verification.MISSING;
    private FutureTask<Void> diskTask;
    private volatile int diskTaskId;
    private boolean recoverOnVerify = true;
    private DiskStatus diskMessage;
    private DiskStatus diskProgress;
    private LauncherScreen launcher;
    private boolean importing;
    private boolean launching;
    private boolean resumed;
    private boolean lastGameRunning;
    private final Runnable refreshGameExit = new Runnable() {
        @Override public void run() {
            if (!resumed || isDestroyed()) return;
            refresh();
            if (lastGameRunning) launcher.status.postDelayed(this, 1000);
        }
    };

    private record DiskStatus(int resource, Object... arguments) {
        String text(Context context) {
            return context.getString(resource, arguments);
        }
    }

    private static final class DiskFailure extends IOException {
        final DiskStatus status;

        DiskFailure(int resource) {
            status = new DiskStatus(resource);
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("game", MODE_PRIVATE);
        discs = new DiscRepository(getFilesDir(), getCacheDir());
        player = new PlayerPackage(getFilesDir());
        canPrepare = BuildConfig.PORTABLE && OnDevicePreparation.isAvailable(this);
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            window.setStatusBarColor(Color.TRANSPARENT);
            window.setNavigationBarColor(Color.TRANSPARENT);
            window.setNavigationBarContrastEnforced(false);
        } else {
            window.setStatusBarColor(LauncherScreen.BACKGROUND);
            window.setNavigationBarColor(LauncherScreen.BACKGROUND);
        }
        launcher = new LauncherScreen(this);
        launcher.playButton.setOnClickListener(view -> play());
        launcher.importButton.setOnClickListener(view -> openPicker(IMPORT_DISC));
        launcher.playerButton.setOnClickListener(view -> openPicker(IMPORT_PLAYER));
        launcher.prepareButton.setOnClickListener(view -> prepareActiveDisc());
        launcher.cancelButton.setOnClickListener(view -> cancelImport());
        launcher.controlsButton.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle(localized.getText(R.string.launcher_touch_title))
                .setMessage(localized.getText(R.string.launcher_touch_help))
                .setPositiveButton(localized.getText(R.string.launcher_ok), null).show());
        launcher.languageButton.setOnClickListener(view -> chooseLanguage());
        setContentView(launcher);
        applyLanguage();
        refresh();
    }

    private void chooseLanguage() {
        new AlertDialog.Builder(this).setTitle(localized.getText(R.string.launcher_language))
                .setSingleChoiceItems(LauncherLanguage.NAMES, LauncherLanguage.indexOf(LauncherLanguage.current(this)),
                        (dialog, index) -> {
                            dialog.dismiss();
                            if (LauncherLanguage.set(this, LauncherLanguage.CODES[index])) {
                                applyLanguage();
                                refresh();
                            } else {
                                new AlertDialog.Builder(this)
                                        .setMessage(localized.getText(R.string.launcher_language_save_failed))
                                        .setPositiveButton(localized.getText(R.string.game_close), null).show();
                            }
                        })
                .setNegativeButton(localized.getText(R.string.game_close), null).show();
    }

    private void applyLanguage() {
        localized = LauncherLanguage.localized(this);
        launcher.applyLanguage(localized, canPrepare);
    }

    private void openPicker(int request) {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(picker, request);
    }

    private boolean hasGame() {
        return BuildConfig.PORTABLE ? playerInstalled : BuildConfig.HAS_GAME;
    }

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        launcher.updateLayout(config);
        applyLanguage();
        refresh();
    }

    private void refresh() {
        boolean ready = verifiedDisc.compatible();
        boolean running = gameRunning();
        lastGameRunning = running;
        boolean busy = diskTask != null;
        boolean canRetry = canPrepare && ready && !playerInstalled && !running;
        CharSequence playLabel = localized.getText(running ? R.string.launcher_continue : R.string.launcher_play);
        if (!playLabel.equals(launcher.playButton.getText())) launcher.playButton.setText(playLabel);
        if (canRetry) {
            launcher.setPrimaryAction(launcher.prepareButton);
        } else if (BuildConfig.PORTABLE && !canPrepare && !playerInstalled && !running) {
            launcher.setPrimaryAction(launcher.playerButton);
        } else {
            launcher.setPrimaryAction(ready || running ? launcher.playButton : launcher.importButton);
        }
        launcher.playButton.setEnabled(ready && hasGame() && !busy && !launching);
        launcher.importButton.setEnabled(!busy && !running);
        launcher.playerButton.setEnabled(!busy && !running);
        launcher.prepareButton.setVisibility(canRetry && !busy ? View.VISIBLE : View.GONE);
        launcher.cancelButton.setVisibility(importing ? View.VISIBLE : View.GONE);
        launcher.progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        DiskStatus status;
        if (busy) status = diskProgress;
        else if (running) status = new DiskStatus(BuildConfig.PORTABLE && !canPrepare
                ? R.string.launcher_player_running : R.string.launcher_disc_running);
        else if (diskMessage != null) status = diskMessage;
        else if (BuildConfig.PORTABLE && !canPrepare && !playerInstalled) {
            status = new DiskStatus(R.string.launcher_player_missing);
        } else if (canRetry) status = new DiskStatus(R.string.launcher_prepare_pending);
        else if (!BuildConfig.PORTABLE && !hasGame()) status = new DiskStatus(R.string.launcher_build_pending);
        else if (ready) status = new DiskStatus(R.string.launcher_disc_ready);
        else if (verifiedDisc.directory() != null) status = new DiskStatus(R.string.launcher_disc_incompatible);
        else status = new DiskStatus(R.string.launcher_disc_missing);
        showStatus(status);
    }

    private void showStatus(DiskStatus status) {
        String text = status.text(localized);
        if (!text.contentEquals(launcher.status.getText())) launcher.status.setText(text);
    }

    private boolean gameRunning() {
        android.app.ActivityManager manager = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes) {
            if (process.processName.equals(getPackageName() + ":game")) return true;
        }
        return false;
    }

    private void play() {
        if (launching || diskTask != null || !hasGame() || !verifiedDisc.compatible()) return;
        launching = true;
        launcher.playButton.setEnabled(false);
        try {
            Intent intent = new Intent(this, NativeGameActivity.class);
            intent.putExtra("disc", verifiedDisc.directory().getAbsolutePath());
            intent.putExtra(LauncherLanguage.EXTRA_LANGUAGE, LauncherLanguage.current(this));
            startActivity(intent);
        } catch (LinkageError | RuntimeException error) {
            launching = false;
            refresh();
            Log.w("SOTC", "Could not start native game", error);
            new AlertDialog.Builder(this).setTitle(localized.getText(R.string.game_start_failed))
                    .setPositiveButton(localized.getText(R.string.game_close), null).show();
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        boolean playerRequest = BuildConfig.PORTABLE && !canPrepare && request == IMPORT_PLAYER;
        if ((!playerRequest && request != IMPORT_DISC) || result != RESULT_OK
                || data == null || data.getData() == null || importing || gameRunning()) return;
        // A picker result can arrive as a recreated activity starts its verification.
        cancelDiskTask();
        Uri uri = data.getData();
        startDiskTask(true, new DiskStatus(playerRequest
                ? R.string.launcher_player_importing : R.string.launcher_disc_importing), id -> {
            if (playerRequest) importPlayer(uri, id);
            else importDisc(uri, id);
        });
    }

    private void startDiskTask(boolean importing, DiskStatus progress, IntConsumer work) {
        int id = ++diskTaskId;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        this.importing = importing;
        diskProgress = progress;
        diskTask = new FutureTask<>(() -> {
            try {
                work.accept(id);
            } catch (RuntimeException error) {
                Log.w("SOTC", "Could not access disc files", error);
                publishDiskResult(id, DiscRepository.Verification.MISSING, false,
                        new DiskStatus(R.string.launcher_disc_access_failed));
            }
        }, null);
        DISK_WORKER.execute(diskTask);
        refresh();
    }

    private void cancelImport() {
        if (diskTask == null || !importing) return;
        cancelDiskTask();
        verifyActiveDisc(new DiskStatus(R.string.launcher_import_cancelled));
    }

    private void cancelDiskTask() {
        if (diskTask != null) {
            diskTaskId++;
            // An interrupted SharedPreferences.commit may still write its pending value.
            boolean interrupt = discCommit.tryLock();
            try {
                diskTask.cancel(interrupt);
            } finally {
                if (interrupt) discCommit.unlock();
            }
            DISK_WORKER.remove(diskTask);
            diskTask = null;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    private void verifyActiveDisc() {
        verifyActiveDisc(null);
    }

    private void verifyActiveDisc(DiskStatus message) {
        startDiskTask(false, new DiskStatus(R.string.launcher_disc_verifying), id -> {
            String activePath = preferences.getString("disc", "");
            if (recoverOnVerify && !gameRunning()) {
                recoverOnVerify = !recoverInterruptedImports(activePath);
            }
            finishDiskTask(id, activePath, message);
        });
    }

    private boolean recoverInterruptedImports(String activePath) {
        try {
            discs.recoverInterruptedImports(activePath);
            if (BuildConfig.PORTABLE) player.recoverInterruptedImports();
            return true;
        } catch (IOException | RuntimeException error) {
            Log.w("SOTC", "Could not clean up interrupted imports", error);
            return false;
        }
    }

    private void finishDiskTask(int id, String activePath, DiskStatus message) {
        if (diskTaskId != id) return;
        DiscRepository.Verification verification;
        try {
            verification = discs.verify(activePath, BOOT_SHA256);
        } catch (IOException | RuntimeException error) {
            Log.w("SOTC", "Could not verify disc files", error);
            verification = DiscRepository.Verification.MISSING;
            if (message == null) message = new DiskStatus(R.string.launcher_disc_verify_failed);
        }
        boolean installed = runnerInstalled();
        if (message != null && message.resource() == R.string.launcher_prepare_ready
                && (!installed || !verification.compatible())) {
            message = new DiskStatus(R.string.launcher_prepare_failed);
        }
        publishDiskResult(id, verification, installed, message);
    }

    private boolean runnerInstalled() {
        return BuildConfig.PORTABLE && (OnDevicePreparation.installedRunner(this) != null
                || player.installedRunner() != null);
    }

    private void publishDiskProgress(int id, DiskStatus progress) {
        runOnUiThread(() -> {
            if (isDestroyed() || diskTaskId != id || diskTask == null) return;
            diskProgress = progress;
            showStatus(progress);
        });
    }

    private void publishDiskResult(int id, DiscRepository.Verification result, boolean playerInstalled,
                                   DiskStatus message) {
        runOnUiThread(() -> {
            if (isDestroyed() || diskTaskId != id) return;
            diskTask = null;
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            importing = false;
            verifiedDisc = result;
            this.playerInstalled = playerInstalled;
            diskMessage = message;
            refresh();
        });
    }

    private void importPlayer(Uri uri, int id) {
        String activePath = preferences.getString("disc", "");
        DiskStatus message;
        try {
            if (gameRunning()) throw new DiskFailure(R.string.launcher_player_running);
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                player.install(input);
            }
            message = new DiskStatus(discs.verify(activePath, BOOT_SHA256).compatible()
                    ? R.string.launcher_player_imported_ready : R.string.launcher_player_imported);
        } catch (DiskFailure error) {
            message = error.status;
        } catch (IOException | RuntimeException error) {
            Log.w("SOTC", "Could not import player package", error);
            message = new DiskStatus(R.string.launcher_player_import_failed);
        }
        finishDiskTask(id, activePath, message);
    }

    private void importDisc(Uri uri, int id) {
        File temporary = new File(getCacheDir(), "import-" + UUID.randomUUID() + ".iso");
        File destination = new File(getFilesDir(), "disc-" + UUID.randomUUID());
        String activePath = preferences.getString("disc", "");
        DiskStatus message;
        boolean imported = false;
        try {
            if (gameRunning()) throw new DiskFailure(R.string.launcher_disc_running);
            recoverOnVerify = !recoverInterruptedImports(activePath);
            byte[] block = new byte[256 * 1024];
            long copied = 0;
            long lastProgress = 0;
            long available = new StatFs(getFilesDir().getAbsolutePath()).getAvailableBytes();
            long maximum = Math.min(8L * 1024 * 1024 * 1024, Math.max(0, (available - 512L * 1024 * 1024) / 2));
            try (InputStream input = getContentResolver().openInputStream(uri);
                 OutputStream output = Files.newOutputStream(temporary.toPath())) {
                if (input == null) throw new DiskFailure(R.string.launcher_disc_open_failed);
                int count;
                while ((count = input.read(block)) != -1) {
                    checkDiskTask(id);
                    copied += count;
                    if (copied > maximum) throw new DiskFailure(R.string.launcher_disc_size_failed);
                    output.write(block, 0, count);
                    long now = System.nanoTime();
                    if (now - lastProgress > 250_000_000L) {
                        lastProgress = now;
                        publishDiskProgress(id, new DiskStatus(R.string.launcher_disc_copying_mb, copied / (1024 * 1024)));
                    }
                }
            }
            final long[] lastExtractionProgress = { 0 };
            DiscImageImporter.extract(temporary, destination, BOOT_SHA256, (bytes, files) -> {
                long now = System.nanoTime();
                if (now - lastExtractionProgress[0] > 250_000_000L) {
                    lastExtractionProgress[0] = now;
                    publishDiskProgress(id, new DiskStatus(R.string.launcher_disc_extracting_mb, bytes / (1024 * 1024)));
                }
            });
            commitImportedDisc(destination, activePath, id);
            activePath = destination.getAbsolutePath();
            imported = true;
            message = new DiskStatus(R.string.launcher_disc_imported);
        } catch (DiskFailure error) {
            message = error.status;
        } catch (IOException | RuntimeException error) {
            Log.w("SOTC", "Could not import PS2 disc", error);
            message = new DiskStatus(R.string.launcher_disc_import_failed);
        } finally {
            temporary.delete();
            if (!gameRunning()) recoverOnVerify = !recoverInterruptedImports(activePath);
        }
        if (imported && BuildConfig.PORTABLE && !runnerInstalled()) {
            message = canPrepare ? prepareDisc(destination, id)
                    : new DiskStatus(R.string.launcher_disc_imported_waiting_for_player);
        }
        finishDiskTask(id, activePath, message);
    }

    private void prepareActiveDisc() {
        if (!canPrepare || diskTask != null || gameRunning() || !verifiedDisc.compatible()) return;
        File directory = verifiedDisc.directory();
        startDiskTask(true, new DiskStatus(R.string.launcher_prepare_starting), id ->
                finishDiskTask(id, directory.getAbsolutePath(), prepareDisc(directory, id)));
    }

    private DiskStatus prepareDisc(File directory, int id) {
        try {
            checkDiskTask(id);
            if (gameRunning()) throw new DiskFailure(R.string.launcher_disc_running);
            publishDiskProgress(id, new DiskStatus(R.string.launcher_prepare_starting));
            File runner = OnDevicePreparation.prepare(this, directory, percent ->
                    publishDiskProgress(id, new DiskStatus(R.string.launcher_prepare_percent, percent)));
            if (runner == null) return new DiskStatus(R.string.launcher_prepare_failed);
            return new DiskStatus(R.string.launcher_prepare_ready);
        } catch (DiskFailure error) {
            return error.status;
        } catch (IOException | RuntimeException | LinkageError error) {
            Log.w("SOTC", "Could not prepare the native game on this device", error);
            return new DiskStatus(R.string.launcher_prepare_failed);
        }
    }

    private void checkDiskTask(int id) throws DiskFailure {
        if (diskTaskId != id || Thread.currentThread().isInterrupted()) {
            throw new DiskFailure(R.string.launcher_import_cancelled);
        }
    }

    private void commitImportedDisc(File destination, String previousPath, int id) throws IOException {
        discCommit.lock();
        try {
            checkDiskTask(id);
            if (gameRunning()) throw new DiskFailure(R.string.launcher_disc_running);
            if (!preferences.edit().putString("disc", destination.getAbsolutePath()).commit()) {
                preferences.edit().putString("disc", previousPath).apply();
                throw new DiskFailure(R.string.launcher_disc_commit_failed);
            }
        } finally {
            discCommit.unlock();
        }
    }

    @Override protected void onDestroy() {
        cancelDiskTask();
        super.onDestroy();
    }
    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        launching = false;
        if (launcher != null) {
            if (diskTask == null) verifyActiveDisc();
            else refresh();
            // The native process may still be draining audio when this screen
            // resumes. Refresh again until it exits, then enable disc replacement.
            launcher.status.removeCallbacks(refreshGameExit);
            launcher.status.postDelayed(refreshGameExit, 500);
        }
    }
    @Override protected void onPause() {
        resumed = false;
        if (launcher != null) launcher.status.removeCallbacks(refreshGameExit);
        super.onPause();
    }
}
