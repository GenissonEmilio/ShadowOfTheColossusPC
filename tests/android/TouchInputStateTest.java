package com.genisson.sotc;

public final class TouchInputStateTest {
    public static void main(String[] args) {
        ps2ButtonBits();
        gripWhileMovingAndJumping();
        slideButtonsAndSharedHolds();
        cameraWithOtherFingers();
        cameraIgnoresControlsAndRebases();
        cameraScalesWithSpeedAndResolution();
        slowCameraDragCrossesGuestDeadZone();
        cancelAndReusePointerIds();
        System.out.println("TouchInputStateTest passed");
    }

    private static void ps2ButtonBits() {
        equal(0x0001, TouchInputState.SELECT, "select bit");
        equal(0x0008, TouchInputState.START, "start bit");
        equal(0x0020, TouchInputState.RIGHT, "d-pad right bit");
        equal(0x0080, TouchInputState.LEFT, "d-pad left bit");
        equal(0x0400, TouchInputState.L1, "L1 bit");
        equal(0x0800, TouchInputState.R1, "R1 bit");
        equal(0x1000, TouchInputState.TRIANGLE, "triangle bit");
        equal(0x2000, TouchInputState.CIRCLE, "circle bit");
        equal(0x4000, TouchInputState.CROSS, "cross bit");
        equal(0x8000, TouchInputState.SQUARE, "square bit");
    }

    private static void gripWhileMovingAndJumping() {
        TouchInputState input = makeInput();
        input.pointerDown(22, 0, 0, 100, TouchInputState.R1, false, false);
        input.pointerDown(3, 55, 45, 101, 0, true, false);
        near(0, input.moveX, "joystick radial dead zone x");
        near(0, input.moveY, "joystick radial dead zone y");
        input.pointerMove(3, 150, 150, 115, 0, false);
        near(.7071068f, input.moveX, "right diagonal clamped radially");
        near(.7071068f, input.moveY, "down diagonal clamped radially");
        input.pointerDown(31, 0, 0, 120, TouchInputState.TRIANGLE, false, false);
        equal(TouchInputState.R1 | TouchInputState.TRIANGLE, input.buttons, "grip and jump");
        input.pointerUp(31);
        equal(TouchInputState.R1, input.buttons, "jump release preserves grip");
        input.pointerUp(3);
        near(0, input.moveX, "joystick release x");
        near(0, input.moveY, "joystick release y");
        equal(TouchInputState.R1, input.buttons, "joystick release preserves grip");
        input.pointerUp(22);
        equal(0, input.buttons, "grip release");
    }

    private static void slideButtonsAndSharedHolds() {
        TouchInputState input = makeInput();
        input.pointerDown(12, 0, 0, 100, TouchInputState.SQUARE, false, false);
        input.pointerDown(23, 0, 0, 102, TouchInputState.SQUARE, false, false);
        input.pointerMove(12, 110, 20, 117, 0, true);
        equal(TouchInputState.SQUARE, input.buttons, "second hold remains on slide out");
        near(0, input.cameraX, "button slide cannot capture camera");
        input.pointerUp(23);
        equal(0, input.buttons, "all holders left the button");
        input.pointerMove(12, 200, 20, 130, TouchInputState.CIRCLE, false);
        equal(TouchInputState.CIRCLE, input.buttons, "slide into action");
        input.pointerMove(12, 230, 20, 145, TouchInputState.CROSS, false);
        equal(TouchInputState.CROSS, input.buttons, "slide from action to Agro");
        input.pointerUp(12);
        equal(0, input.buttons, "button pointer release");
    }

    private static void cameraWithOtherFingers() {
        TouchInputState input = makeInput();
        input.pointerDown(3, 50, 50, 100, 0, true, false);
        input.pointerDown(9, 400, 50, 100, 0, false, true);
        input.pointerDown(18, 470, 60, 101, 0, false, true);
        input.pointerMove(18, 500, 105, 105, TouchInputState.R1, false);
        input.pointerMove(3, 15, 50, 116, 0, false);
        input.pointerMove(9, 408, 54, 116, 0, true);
        near(-.66171515f, input.cameraX, "rightward camera drag");
        near(-.33085757f, input.cameraY, "downward camera drag");
        equal(TouchInputState.R1, input.buttons, "second free finger may slide into grip");
        input.pointerMove(9, 408, 54, 150, 0, true);
        input.expireCamera(163);
        near(-.66171515f, input.cameraX, "latest camera sample remains briefly available");
        input.expireCamera(164);
        near(0, input.cameraX, "stationary camera zeros x");
        near(0, input.cameraY, "stationary camera zeros y");
        equal(TouchInputState.R1, input.buttons, "camera timeout preserves grip");
        if (input.moveX >= 0) {
            throw new AssertionError("camera timeout cleared left joystick");
        }
        input.pointerMove(9, 420, 54, 171, 0, true);
        input.pointerUp(9);
        near(0, input.cameraX, "camera release zeros latest movement immediately");
        equal(TouchInputState.R1, input.buttons, "camera release preserves grip");
    }

    private static void cameraIgnoresControlsAndRebases() {
        TouchInputState input = makeInput();
        input.pointerDown(13, 580, 70, 100, TouchInputState.TRIANGLE, false, false);
        input.pointerMove(13, 500, 70, 116, 0, true);
        near(0, input.cameraX, "gesture begun on triangle remains a button gesture");
        equal(0, input.buttons, "sliding off the triangle releases it");
        input.pointerDown(14, 500, 70, 120, 0, false, true);
        input.pointerMove(14, 600, 70, 133, TouchInputState.CIRCLE, false);
        near(0, input.cameraX, "camera stops over button");
        equal(0, input.buttons, "camera finger cannot press a button");
        input.pointerMove(14, 450, 80, 146, 0, true);
        near(0, input.cameraX, "leaving button does not cause camera jump");
        input.pointerMove(14, 442, 72, 162, 0, true);
        near(.58491378f, input.cameraX, "left camera drag resumes");
        near(.58491378f, input.cameraY, "up camera drag resumes");
    }

    private static void cameraScalesWithSpeedAndResolution() {
        TouchInputState input = makeInput();
        input.pointerDown(17, 500, 200, 100, 0, false, true);
        input.pointerMove(17, 508, 196, 116, 0, true);
        float lowResolutionCameraX = input.cameraX;
        float lowResolutionCameraY = input.cameraY;

        input.reset();
        input.configureGeometry(100, 100, 200, 2000);
        input.pointerDown(17, 1000, 400, 100, 0, false, true);
        input.pointerMove(17, 1032, 384, 132, 0, true);
        near(lowResolutionCameraX, input.cameraX, "camera x uses relative screen speed");
        near(lowResolutionCameraY, input.cameraY, "camera y uses relative screen speed");

        input.configureCamera(2, true, true);
        input.pointerMove(17, 1048, 376, 148, 0, true);
        near(.89442719f, input.cameraX, "inverted x with doubled sensitivity");
        near(-.44721359f, input.cameraY, "inverted y with doubled sensitivity");
        input.pointerMove(17, 1120, 500, 156, 0, true);
        near(1, (float) Math.hypot(input.cameraX, input.cameraY), "camera magnitude saturates");
        near(72f / 124f, input.cameraX / input.cameraY, "camera retains direction at maximum speed");
    }

    private static void slowCameraDragCrossesGuestDeadZone() {
        TouchInputState input = makeInput();
        input.configureGeometry(50, 50, 100, 1220);
        input.pointerDown(17, 500, 200, 100, 0, false, true);
        near(0, input.cameraX, "camera is neutral on contact");
        input.pointerMove(17, 508, 200, 118, 0, true);
        near(-.62493625f, input.cameraX, "slow swipe is above the game's dead zone");
        near(0, input.cameraY, "horizontal swipe stays horizontal");
        input.pointerMove(17, 508.01f, 200, 136, 0, true);
        int rawStickX = Math.round(128 + input.cameraX * 128);
        if (Math.abs(rawStickX * (2f / 255) - 1) < .4f) {
            throw new AssertionError("8-bit axis quantization put a slow swipe below the game's dead zone");
        }
        input.pointerUp(17);
        near(0, input.cameraX, "camera returns to neutral on release");
    }

    private static void cancelAndReusePointerIds() {
        TouchInputState input = makeInput();
        input.pointerDown(5, 0, 0, 100, TouchInputState.R1, false, false);
        input.pointerDown(1, 85, 80, 110, 0, true, false);
        input.pointerDown(8, 600, 80, 120, 0, false, true);
        input.pointerMove(8, 620, 100, 135, 0, true);
        input.reset();
        input.pointerMove(5, 0, 0, 140, TouchInputState.R1, false);
        input.pointerMove(1, 85, 80, 141, 0, true);
        input.pointerMove(8, 620, 105, 142, 0, true);
        equal(0, input.buttons, "cancel clears grip and ignores stale moves");
        near(0, input.moveX, "cancel clears left x");
        near(0, input.moveY, "cancel clears left y");
        near(0, input.cameraX, "cancel clears right x");
        near(0, input.cameraY, "cancel clears right y");
        input.pointerDown(5, 50, 50, 150, 0, true, false);
        input.pointerMove(5, -80, 50, 170, TouchInputState.R1, false);
        near(-1, input.moveX, "pointer ID reused by joystick");
        equal(0, input.buttons, "joystick drag cannot press outside buttons");
    }

    private static TouchInputState makeInput() {
        TouchInputState input = new TouchInputState();
        input.configureGeometry(50, 50, 100, 1000);
        return input;
    }

    private static void equal(int expected, int actual, String message) {
        if (expected != actual) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }

    private static void near(float expected, float actual, String message) {
        if (Math.abs(expected - actual) > .00001f || !Float.isFinite(actual)) {
            throw new AssertionError(message + ": expected " + expected + ", got " + actual);
        }
    }
}
