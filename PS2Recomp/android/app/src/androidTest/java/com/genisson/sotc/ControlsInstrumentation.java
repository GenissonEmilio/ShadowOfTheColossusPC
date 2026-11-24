package com.genisson.sotc;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public final class ControlsInstrumentation extends Instrumentation {
    private Bundle arguments;

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        this.arguments = arguments == null ? Bundle.EMPTY : arguments;
        start();
    }

    @Override
    public void onStart() {
        Bundle result = new Bundle();
        int tests = 0;
        Activity activity = null;
        SharedPreferences preferences = getTargetContext().getSharedPreferences(
                "sotc-touch-controls-instrumentation", Context.MODE_PRIVATE);
        try {
            check(preferences.edit().clear().commit(), "could not initialize test preferences");
            Intent intent = new Intent(getTargetContext(), MainActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            activity = startActivitySync(intent);
            Activity launched = activity;
            onMain(() -> launched.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE));
            Probe probe = new Probe();
            TouchControlsView view = showHud(activity, preferences, probe);
            verifyReadableLabels(view);
            tests++;
            verifyActionLayout(view, probe);
            tests++;
            verifySimultaneousControlsAndIdle(view, probe);
            tests++;
            verifyButtonGesture(view, probe);
            tests++;
            verifyLayoutPersistence(activity, view, preferences, probe);
            tests++;
            if (BuildConfig.PORTABLE) {
                PlayerPackageInstrumentation.verify(getTargetContext());
                tests++;
            }
            if (Boolean.parseBoolean(arguments.getString("screenshot"))) {
                result.putString("screenshot", takeScreenshot(activity));
            }
            result.putString("stream", "\nControlsInstrumentation passed " + tests + " checks.\n");
        } catch (Throwable failure) {
            String error = Log.getStackTraceString(failure);
            result.putString("failure", error);
            result.putString("stream", "\nControlsInstrumentation failed.\n" + error + "\n");
        } finally {
            if (activity != null) {
                Activity launched = activity;
                runOnMainSync(launched::finish);
            }
            preferences.edit().clear().commit();
        }
        result.putInt("tests", tests);
        finish(result.containsKey("failure") ? Activity.RESULT_CANCELED : Activity.RESULT_OK, result);
    }

    private TouchControlsView showHud(Activity activity, SharedPreferences preferences, Probe probe)
            throws Exception {
        AtomicReference<TouchControlsView> view = new AtomicReference<>();
        CountDownLatch layout = new CountDownLatch(1);
        onMain(() -> {
            TouchControlsView controls = new TouchControlsView(activity, preferences);
            controls.setBackgroundColor(0xff293b37);
            controls.setListener(probe);
            controls.addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                if (bottom > top && right - left > bottom - top) {
                    layout.countDown();
                }
            });
            view.set(controls);
            activity.setContentView(controls);
        });
        check(layout.await(5, TimeUnit.SECONDS), "HUD did not lay out in landscape");
        waitForIdleSync();
        return view.get();
    }

    private void verifySimultaneousControlsAndIdle(TouchControlsView view, Probe probe) throws Exception {
        TouchSequence touch = new TouchSequence(view);
        Contact[] contacts = new Contact[3];
        onMain(() -> {
            RectF stick = view.controlBounds(0);
            Contact move = Contact.at(19, stick);
            Contact grip = Contact.at(3, view.controlBounds(TouchInputState.R1));
            Contact camera = new Contact(27, view.getWidth() * .69f, view.getHeight() * .40f);
            float travel = Math.min(view.getWidth(), view.getHeight()) * .035f;
            touch.send(MotionEvent.ACTION_DOWN, move);
            touch.send(pointerAction(MotionEvent.ACTION_POINTER_DOWN, 1), move, grip);
            touch.send(pointerAction(MotionEvent.ACTION_POINTER_DOWN, 2), move, grip, camera);
            contacts[0] = move.offset(stick.width() * .32f, stick.height() * .18f);
            contacts[1] = grip;
            contacts[2] = camera.offset(travel, travel);
            touch.send(MotionEvent.ACTION_MOVE, contacts);
            check(probe.buttons == TouchInputState.R1, "grip is not active alongside both sticks");
            check(probe.moveX > 0 && probe.moveY > 0, "joystick did not move down and right");
            check(probe.cameraX < 0 && probe.cameraY < 0, "free swipe did not move the camera in the expected direction");
            check(Math.hypot(probe.cameraX, probe.cameraY) > .4, "free swipe stayed inside the game's dead zone");
            contacts[2] = Contact.at(camera.id, view.controlBounds(TouchInputState.CIRCLE));
            touch.send(MotionEvent.ACTION_MOVE, contacts);
            check(Math.hypot(probe.cameraX, probe.cameraY) > .4 && probe.buttons == TouchInputState.R1,
                    "camera drag stopped or pressed Action when crossing its button");
        });
        check(probe.cameraStopped.await(3, TimeUnit.SECONDS), "stationary finger kept the camera active");
        onMain(() -> {
            check(probe.buttons == TouchInputState.R1 && probe.moveX > 0,
                    "camera timeout released grip or joystick");
            check(probe.cameraX == 0 && probe.cameraY == 0, "camera was not neutral after the idle delay");
            touch.send(pointerAction(MotionEvent.ACTION_POINTER_UP, 1), contacts);
            check(probe.buttons == 0 && probe.moveX > 0, "lifting the middle pointer released the wrong control");
            float adjustment = view.controlBounds(0).width() * .08f;
            touch.send(MotionEvent.ACTION_MOVE, contacts[2], contacts[0].offset(-adjustment, 0));
            check(probe.moveX > 0 && probe.cameraX == 0, "pointer index change confused the joystick with the camera");
            touch.send(MotionEvent.ACTION_CANCEL, contacts[2], contacts[0]);
            check(probe.isNeutral(), "cancellation left the joystick, camera or a button active");
        });
    }

    private void verifyActionLayout(TouchControlsView view, Probe probe) throws Exception {
        onMain(() -> {
            RectF grip = view.controlBounds(TouchInputState.R1);
            RectF action = view.controlBounds(TouchInputState.CIRCLE);
            RectF attack = view.controlBounds(TouchInputState.SQUARE);
            RectF agro = view.controlBounds(TouchInputState.CROSS);
            RectF jump = view.controlBounds(TouchInputState.TRIANGLE);
            check(grip.centerY() < action.centerY() && action.centerY() < attack.centerY()
                            && attack.centerY() < agro.centerY() && agro.centerY() < jump.centerY(),
                    "default action buttons are out of order from top to bottom");
            check(agro.centerX() < action.centerX() && action.centerX() < jump.centerX()
                            && jump.centerX() < attack.centerX() && attack.centerX() < grip.centerX(),
                    "default action buttons are out of order from left to right");
            int[] buttons = {TouchInputState.R1, TouchInputState.CIRCLE, TouchInputState.SQUARE,
                    TouchInputState.CROSS, TouchInputState.TRIANGLE};
            TouchSequence touch = new TouchSequence(view);
            for (int button : buttons) {
                Contact contact = Contact.at(12, view.controlBounds(button));
                touch.send(MotionEvent.ACTION_DOWN, contact);
                check(probe.buttons == button, "action button sent the wrong input: " + button);
                touch.send(MotionEvent.ACTION_UP, contact);
                check(probe.isNeutral(), "action button left its input held: " + button);
            }
        });
    }

    private void verifyReadableLabels(TouchControlsView view) throws Exception {
        onMain(() -> {
            Bitmap image = Bitmap.createBitmap(view.getWidth(), view.getHeight(), Bitmap.Config.ARGB_8888);
            try {
                view.draw(new android.graphics.Canvas(image));
                RectF bounds = view.controlBounds(TouchInputState.R1);
                float radius = bounds.width() / 2;
                int minimum = image.getWidth();
                int maximum = -1;
                for (int y = (int) (bounds.centerY() + radius * .28f);
                        y < bounds.centerY() + radius * .73f; y++) {
                    for (int x = (int) (bounds.centerX() - radius * .84f);
                            x < bounds.centerX() + radius * .84f; x++) {
                        int color = image.getPixel(x, y);
                        if (android.graphics.Color.red(color) > 170
                                && android.graphics.Color.green(color) > 150
                                && android.graphics.Color.blue(color) > 120) {
                            minimum = Math.min(minimum, x);
                            maximum = Math.max(maximum, x);
                        }
                    }
                }
                check(maximum - minimum > radius * .9f,
                        "Agarrar label collapsed into overlapping glyphs");
            } finally {
                image.recycle();
            }
        });
    }

    private void verifyButtonGesture(TouchControlsView view, Probe probe) throws Exception {
        onMain(() -> {
            TouchSequence touch = new TouchSequence(view);
            Contact jump = Contact.at(15, view.controlBounds(TouchInputState.TRIANGLE));
            Contact free = new Contact(15, view.getWidth() * .69f, view.getHeight() * .40f);
            touch.send(MotionEvent.ACTION_DOWN, jump);
            check(probe.buttons == TouchInputState.TRIANGLE, "touch on the jump control did not press it");
            touch.send(MotionEvent.ACTION_MOVE, free);
            touch.send(MotionEvent.ACTION_MOVE, free.offset(0, -20));
            check(probe.isNeutral(), "drag starting on a button released it but activated another input");
            touch.send(MotionEvent.ACTION_MOVE, jump);
            check(probe.buttons == TouchInputState.TRIANGLE, "sliding back did not press jump again");
            touch.send(MotionEvent.ACTION_CANCEL, jump);
            check(probe.isNeutral(), "button stayed pressed after cancellation");
        });
    }

    private void verifyLayoutPersistence(Activity activity, TouchControlsView view,
            SharedPreferences preferences, Probe probe) throws Exception {
        RectF defaults = new RectF();
        RectF moved = new RectF();
        onMain(() -> {
            defaults.set(view.controlBounds(TouchInputState.TRIANGLE));
            view.setEditing(true);
            check(view.isEditing(), "could not enter edit mode");
            TouchSequence touch = new TouchSequence(view);
            Contact start = Contact.at(25, defaults);
            Contact end = start.offset(-defaults.width() * .5f, -defaults.height() * .65f);
            touch.send(MotionEvent.ACTION_DOWN, start);
            touch.send(MotionEvent.ACTION_MOVE, end);
            touch.send(MotionEvent.ACTION_UP, end);
            check(probe.isNeutral(), "edit gesture pressed a game control");
            moved.set(view.controlBounds(TouchInputState.TRIANGLE));
            check(defaults.centerX() - moved.centerX() > defaults.width() * .4f,
                    "drag did not move jump within the HUD");
            view.setEditing(false);
            view.setControlScale(1.18f);
            view.setOpacity(.55f);
            view.setCameraSensitivity(1.7f);
            view.setInvertCamera(true, false);
        });

        TouchControlsView reloaded = showHud(activity, preferences, probe);
        onMain(() -> {
            RectF position = reloaded.controlBounds(TouchInputState.TRIANGLE);
            check(near(position.centerX(), moved.centerX()) && near(position.centerY(), moved.centerY()),
                    "new HUD did not restore the edited position");
            check(reloaded.getControlScale() == 1.18f && reloaded.getOpacity() == .55f
                            && reloaded.getCameraSensitivity() == 1.7f && reloaded.isCameraXInverted()
                            && !reloaded.isCameraYInverted(), "new HUD did not restore tuning");
            reloaded.resetLayout();
            check(reloaded.getControlScale() == 1.18f && reloaded.getOpacity() == .55f,
                    "layout reset changed tuning");
            reloaded.resetSettings();
            RectF restored = reloaded.controlBounds(TouchInputState.TRIANGLE);
            check(near(restored.centerX(), defaults.centerX()) && near(restored.centerY(), defaults.centerY())
                    && near(restored.width(), defaults.width()), "full reset did not restore default geometry");
            check(reloaded.getControlScale() == 1 && reloaded.getCameraSensitivity() == 1
                            && !reloaded.isCameraXInverted() && !reloaded.isCameraYInverted(),
                    "full reset did not restore default tuning");
        });
    }

    private String takeScreenshot(Activity activity) throws IOException {
        SystemClock.sleep(700);
        waitForIdleSync();
        // Copy only our app's surface, without notifications or the OEM's
        // animated display-rotation transform.
        Bitmap image = Bitmap.createBitmap(activity.getWindow().getDecorView().getWidth(),
                activity.getWindow().getDecorView().getHeight(), Bitmap.Config.ARGB_8888);
        CountDownLatch copied = new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicInteger copyResult = new java.util.concurrent.atomic.AtomicInteger(-1);
        android.view.PixelCopy.request(activity.getWindow(), image, code -> {
            copyResult.set(code);
            copied.countDown();
        }, new android.os.Handler(android.os.Looper.getMainLooper()));
        try {
            check(copied.await(3, TimeUnit.SECONDS) && copyResult.get() == android.view.PixelCopy.SUCCESS,
                    "app surface screenshot unavailable: " + copyResult.get());
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IOException("screenshot interrupted", error);
        }
        File directory = activity.getExternalFilesDir(null);
        File destination = new File(directory == null ? activity.getFilesDir() : directory, "hud-test.png");
        try (FileOutputStream output = new FileOutputStream(destination)) {
            check(image.compress(Bitmap.CompressFormat.PNG, 100, output), "could not encode HUD screenshot");
        } finally {
            image.recycle();
        }
        return destination.getAbsolutePath();
    }

    private void onMain(Runnable check) throws Exception {
        FutureTask<Void> task = new FutureTask<>(check, null);
        runOnMainSync(task);
        try {
            task.get();
        } catch (ExecutionException error) {
            throw new AssertionError("HUD check failed on the UI thread", error.getCause());
        }
    }

    private static int pointerAction(int action, int index) {
        return action | (index << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) < 1;
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static final class Probe implements TouchControlsView.Listener {
        final CountDownLatch cameraStopped = new CountDownLatch(1);
        int buttons;
        float moveX;
        float moveY;
        float cameraX;
        float cameraY;

        @Override
        public void onInput(int buttons, float moveX, float moveY, float cameraX, float cameraY) {
            boolean cameraWasMoving = this.cameraX != 0 || this.cameraY != 0;
            this.buttons = buttons;
            this.moveX = moveX;
            this.moveY = moveY;
            this.cameraX = cameraX;
            this.cameraY = cameraY;
            if (cameraWasMoving && cameraX == 0 && cameraY == 0 && buttons == TouchInputState.R1 && moveX != 0) {
                cameraStopped.countDown();
            }
        }

        @Override
        public void onMenu() { }

        boolean isNeutral() {
            return buttons == 0 && moveX == 0 && moveY == 0 && cameraX == 0 && cameraY == 0;
        }
    }

    private static final class TouchSequence {
        private final TouchControlsView view;
        private long downTime;

        TouchSequence(TouchControlsView view) {
            this.view = view;
        }

        void send(int action, Contact... contacts) {
            MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[contacts.length];
            MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[contacts.length];
            for (int index = 0; index < contacts.length; index++) {
                Contact contact = contacts[index];
                MotionEvent.PointerProperties pointer = new MotionEvent.PointerProperties();
                pointer.id = contact.id;
                pointer.toolType = MotionEvent.TOOL_TYPE_FINGER;
                properties[index] = pointer;
                MotionEvent.PointerCoords coords = new MotionEvent.PointerCoords();
                coords.x = contact.x;
                coords.y = contact.y;
                coords.pressure = 1;
                coords.size = 1;
                coordinates[index] = coords;
            }
            long now = SystemClock.uptimeMillis();
            if (action == MotionEvent.ACTION_DOWN) {
                downTime = now;
            }
            MotionEvent event = MotionEvent.obtain(downTime, now, action, contacts.length, properties,
                    coordinates, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
            try {
                check(view.dispatchTouchEvent(event), "HUD did not consume motion action " + action);
            } finally {
                event.recycle();
            }
        }
    }

    private static final class Contact {
        final int id;
        final float x;
        final float y;

        Contact(int id, float x, float y) {
            this.id = id;
            this.x = x;
            this.y = y;
        }

        static Contact at(int id, RectF bounds) {
            return new Contact(id, bounds.centerX(), bounds.centerY());
        }

        Contact offset(float dx, float dy) {
            return new Contact(id, x + dx, y + dy);
        }
    }
}
