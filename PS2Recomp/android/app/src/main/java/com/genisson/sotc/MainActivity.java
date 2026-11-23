package com.genisson.sotc;

import android.app.Activity;
import android.app.AlertDialog;
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
    private static final ThreadPoolExecutor DISK_WORKER = new ThreadPoolExecutor(
            1, 1, 30, TimeUnit.SECONDS, new LinkedBlockingQueue<>());
    static { DISK_WORKER.allowCoreThreadTimeOut(true); }

    private SharedPreferences preferences;
    private DiscRepository discs;
    private final ReentrantLock discCommit = new ReentrantLock();
    private DiscRepository.Verification verifiedDisc = DiscRepository.Verification.MISSING;
    private FutureTask<Void> diskTask;
    private int diskTaskId;
    private boolean recoverOnVerify = true;
    private String diskMessage;
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

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("game", MODE_PRIVATE);
        discs = new DiscRepository(getFilesDir(), getCacheDir());
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
        launcher.importButton.setOnClickListener(view -> {
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            picker.addCategory(Intent.CATEGORY_OPENABLE);
            picker.setType("*/*");
            picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(picker, IMPORT_DISC);
        });
        launcher.controlsButton.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle("Controles touch")
                .setMessage("Mova Wander com o analógico esquerdo. Arraste a área livre à direita para olhar ao redor.\n\n"
                        + "Segure AGARRAR para escalar e combine com PULAR. Use ATACAR para a espada ou arco; AGRO chama ou estimula o cavalo.\n\n"
                        + "Durante o jogo, toque no sigilo em losango para mover e redimensionar botões, ajustar a transparência e a sensibilidade da câmera.")
                .setPositiveButton("ENTENDI", null).show());
        setContentView(launcher);
        refresh();
    }

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        launcher.updateLayout(config);
    }

    private void refresh() {
        boolean ready = verifiedDisc.compatible();
        boolean running = gameRunning();
        lastGameRunning = running;
        boolean busy = diskTask != null;
        launcher.playButton.setText(running ? R.string.launcher_continue : R.string.launcher_play);
        launcher.setImportHighlighted(!ready && !running);
        launcher.playButton.setEnabled(ready && BuildConfig.HAS_GAME && !busy && !launching);
        launcher.importButton.setEnabled(!busy && !running);
        launcher.progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        if (importing) return;
        if (busy) launcher.status.setText("Verificando os arquivos do disco…");
        else if (diskMessage != null) launcher.status.setText(diskMessage);
        else if (!BuildConfig.HAS_GAME) launcher.status.setText("Preparação do jogo pendente nesta compilação.");
        else if (running) launcher.status.setText("A jornada está em andamento. Saia do jogo antes de trocar o disco.");
        else if (ready) launcher.status.setText("Seu disco está pronto. A jornada pode começar.");
        else if (verifiedDisc.directory() != null) launcher.status.setText("O disco importado difere desta compilação. Importe a edição compatível.");
        else launcher.status.setText("Importe sua ISO americana para começar.");
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
        if (launching || diskTask != null || !BuildConfig.HAS_GAME || !verifiedDisc.compatible()) return;
        launching = true;
        launcher.playButton.setEnabled(false);
        try {
            Intent intent = new Intent(this, NativeGameActivity.class);
            intent.putExtra("disc", verifiedDisc.directory().getAbsolutePath());
            startActivity(intent);
        } catch (LinkageError | RuntimeException error) {
            launching = false;
            refresh();
            new AlertDialog.Builder(this).setTitle("Não foi possível iniciar")
                    .setMessage(error.getMessage()).setPositiveButton("FECHAR", null).show();
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == IMPORT_DISC && result == RESULT_OK && data != null && data.getData() != null && !importing && !gameRunning()) {
            // A picker result can arrive as a recreated activity starts its verification.
            cancelDiskTask();
            launcher.status.setText("Lendo seu disco… mantenha o aplicativo aberto.");
            Uri uri = data.getData();
            startDiskTask(true, id -> importDisc(uri, id));
        }
    }

    private void startDiskTask(boolean importing, IntConsumer work) {
        int id = ++diskTaskId;
        this.importing = importing;
        diskTask = new FutureTask<>(() -> {
            try {
                work.accept(id);
            } catch (RuntimeException error) {
                publishDiskResult(id, DiscRepository.Verification.MISSING,
                        "Não foi possível acessar os arquivos do disco: " + error.getMessage());
            }
        }, null);
        DISK_WORKER.execute(diskTask);
        refresh();
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
        }
    }

    private void verifyActiveDisc() {
        startDiskTask(false, id -> {
            String activePath = preferences.getString("disc", "");
            if (recoverOnVerify && !gameRunning()) {
                recoverOnVerify = !recoverInterruptedImports(activePath);
            }
            finishDiskTask(id, activePath, null);
        });
    }

    private boolean recoverInterruptedImports(String activePath) {
        try {
            discs.recoverInterruptedImports(activePath);
            return true;
        } catch (IOException | RuntimeException error) {
            Log.w("SOTC", "Could not clean up interrupted imports", error);
            return false;
        }
    }

    private void finishDiskTask(int id, String activePath, String message) {
        DiscRepository.Verification verification;
        String statusText = message;
        try {
            verification = discs.verify(activePath, BuildConfig.BOOT_SHA256);
        } catch (IOException | RuntimeException error) {
            verification = DiscRepository.Verification.MISSING;
            if (statusText == null) statusText = "Não foi possível verificar o disco: " + error.getMessage();
        }
        publishDiskResult(id, verification, statusText);
    }

    private void publishDiskResult(int id, DiscRepository.Verification result, String message) {
        runOnUiThread(() -> {
            if (isDestroyed() || diskTaskId != id) return;
            diskTask = null;
            importing = false;
            verifiedDisc = result;
            diskMessage = message;
            refresh();
        });
    }

    private void importDisc(Uri uri, int id) {
        File temporary = new File(getCacheDir(), "import-" + UUID.randomUUID() + ".iso");
        File destination = new File(getFilesDir(), "disc-" + UUID.randomUUID());
        String activePath = preferences.getString("disc", "");
        String message;
        try {
            if (gameRunning()) throw new IOException("Saia do jogo antes de trocar o disco.");
            recoverOnVerify = !recoverInterruptedImports(activePath);
            byte[] block = new byte[256 * 1024];
            long copied = 0;
            long available = new StatFs(getFilesDir().getAbsolutePath()).getAvailableBytes();
            long maximum = Math.min(8L * 1024 * 1024 * 1024, Math.max(0, (available - 512L * 1024 * 1024) / 2));
            try (InputStream input = getContentResolver().openInputStream(uri);
                 OutputStream output = Files.newOutputStream(temporary.toPath())) {
                if (input == null) throw new IOException("Não foi possível abrir o arquivo selecionado.");
                int count;
                while ((count = input.read(block)) != -1) {
                    if (Thread.currentThread().isInterrupted()) throw new IOException("Importação cancelada.");
                    copied += count;
                    if (copied > maximum) throw new IOException("Espaço insuficiente ou imagem maior que o suportado.");
                    output.write(block, 0, count);
                }
            }
            final long[] lastProgress = { 0 };
            DiscImageImporter.extract(temporary, destination, BuildConfig.BOOT_SHA256, (bytes, files) -> {
                long now = System.nanoTime();
                if (now - lastProgress[0] > 250_000_000L) {
                    lastProgress[0] = now;
                    runOnUiThread(() -> {
                        if (!isDestroyed() && diskTaskId == id) {
                            launcher.status.setText("Preparando seu disco… " + bytes / (1024 * 1024) + " MB");
                        }
                    });
                }
            });
            commitImportedDisc(destination, activePath);
            activePath = destination.getAbsolutePath();
            message = "Disco importado. Seus saves foram preservados.";
        } catch (IOException | RuntimeException error) {
            message = "Não foi possível importar: " + error.getMessage();
        } finally {
            temporary.delete();
            if (!gameRunning()) recoverOnVerify = !recoverInterruptedImports(activePath);
        }
        finishDiskTask(id, activePath, message);
    }

    private void commitImportedDisc(File destination, String previousPath) throws IOException {
        discCommit.lock();
        try {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Importação cancelada.");
            if (gameRunning()) throw new IOException("Saia do jogo antes de trocar o disco.");
            if (!preferences.edit().putString("disc", destination.getAbsolutePath()).commit()) {
                preferences.edit().putString("disc", previousPath).apply();
                throw new IOException("Não foi possível guardar a importação.");
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
