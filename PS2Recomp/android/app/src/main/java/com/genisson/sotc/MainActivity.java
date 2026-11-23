package com.genisson.sotc;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
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
    private TextView status;
    private Button importButton, playButton;
    private ProgressBar progress;
    private boolean importing;
    private boolean launching;
    private boolean resumed;
    private boolean lastGameRunning;
    private final Runnable refreshGameExit = new Runnable() {
        @Override public void run() {
            if (!resumed || isDestroyed()) return;
            refresh();
            if (lastGameRunning) status.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("game", MODE_PRIVATE);
        discs = new DiscRepository(getFilesDir(), getCacheDir());
        getWindow().setStatusBarColor(Color.rgb(20, 30, 32));
        getWindow().setNavigationBarColor(Color.rgb(20, 30, 32));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER);
        page.setPadding(dp(28), dp(36), dp(28), dp(36));
        page.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] { 0xff263b3f, 0xff111a1d }));
        scroll.addView(page);
        TextView eyebrow = text("TERRAS PROIBIDAS", 13, 0xffb9a276);
        eyebrow.setLetterSpacing(.22f);
        page.addView(eyebrow);
        TextView title = text("SHADOW\nOF THE COLOSSUS", 30, 0xffeee5d2);
        title.setTypeface(Typeface.create("serif", Typeface.BOLD));
        title.setPadding(0, dp(20), 0, dp(16));
        page.addView(title);
        page.addView(text("Explore. Escale. Enfrente os colossos.", 16, 0xffb9c4c3));
        status = text("", 15, 0xffddd6c6);
        status.setPadding(0, dp(28), 0, dp(16));
        page.addView(status);
        progress = new ProgressBar(this);
        progress.setVisibility(View.GONE);
        page.addView(progress, new LinearLayout.LayoutParams(dp(32), dp(32)));
        playButton = button("JOGAR", page);
        playButton.setOnClickListener(view -> play());
        importButton = button("IMPORTAR MEU DISCO", page);
        importButton.setOnClickListener(view -> {
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            picker.addCategory(Intent.CATEGORY_OPENABLE);
            picker.setType("*/*");
            picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(picker, IMPORT_DISC);
        });
        Button controls = button("CONTROLES", page);
        controls.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle("Controles touch")
                .setMessage("Mova Wander com o analógico esquerdo. Arraste a área livre à direita para olhar ao redor.\n\n"
                        + "Segure AGARRAR para escalar e combine com PULAR. Use ATACAR para a espada ou arco; AGRO chama ou estimula o cavalo.\n\n"
                        + "Durante o jogo, toque no sigilo em losango para mover e redimensionar botões, ajustar a transparência e a sensibilidade da câmera.")
                .setPositiveButton("ENTENDI", null).show());
        page.addView(text("Use seu disco de PS2 • Edição americana SCUS-97472\n"
                + "Seus saves ficam separados dos arquivos do jogo.", 12, 0xff91a09f));
        setContentView(scroll);
        refresh();
    }

    private void refresh() {
        boolean ready = verifiedDisc.compatible();
        boolean running = gameRunning();
        lastGameRunning = running;
        boolean busy = diskTask != null;
        playButton.setText(running ? "CONTINUAR" : "JOGAR");
        playButton.setEnabled(ready && BuildConfig.HAS_GAME && !busy && !launching);
        importButton.setEnabled(!busy && !running);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        if (importing) return;
        if (busy) status.setText("Verificando os arquivos do disco…");
        else if (diskMessage != null) status.setText(diskMessage);
        else if (!BuildConfig.HAS_GAME) status.setText("Preparação do jogo pendente nesta compilação.");
        else if (running) status.setText("A jornada está em andamento. Saia do jogo antes de trocar o disco.");
        else if (ready) status.setText("Seu disco está pronto. A jornada pode começar.");
        else if (verifiedDisc.directory() != null) status.setText("O disco importado difere desta compilação. Importe a edição compatível.");
        else status.setText("Importe sua ISO americana para começar.");
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
        playButton.setEnabled(false);
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
            status.setText("Lendo seu disco… mantenha o aplicativo aberto.");
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
                            status.setText("Preparando seu disco… " + bytes / (1024 * 1024) + " MB");
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
        if (status != null) {
            if (diskTask == null) verifyActiveDisc();
            else refresh();
            // The native process may still be draining audio when this screen
            // resumes. Refresh again until it exits, then enable disc replacement.
            status.removeCallbacks(refreshGameExit);
            status.postDelayed(refreshGameExit, 500);
        }
    }
    @Override protected void onPause() {
        resumed = false;
        if (status != null) status.removeCallbacks(refreshGameExit);
        super.onPause();
    }
    private TextView text(String content, int size, int color) {
        TextView view = new TextView(this);
        view.setText(content); view.setTextSize(size); view.setTextColor(color);
        view.setGravity(Gravity.CENTER);
        return view;
    }
    private Button button(String content, LinearLayout parent) {
        Button view = new Button(this);
        view.setText(content); view.setTextColor(0xfff0e7d3);
        view.setPadding(dp(12), dp(12), dp(12), dp(12));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff304346); background.setCornerRadius(dp(8));
        background.setStroke(dp(1), 0xff8d7d5e); view.setBackground(background);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, dp(58));
        params.topMargin = dp(12); params.bottomMargin = dp(8);
        parent.addView(view, params);
        return view;
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
}
