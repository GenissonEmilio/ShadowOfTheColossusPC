package com.genisson.sotc;

import java.util.Arrays;

final class TouchInputState {
    static final int SELECT = 1;
    static final int START = 1 << 3;
    static final int MAP = START;
    static final int RIGHT = 1 << 5;
    static final int LEFT = 1 << 7;
    static final int L1 = 1 << 10;
    static final int R1 = 1 << 11;
    static final int TRIANGLE = 1 << 12;
    static final int CIRCLE = 1 << 13;
    static final int CROSS = 1 << 14;
    static final int SQUARE = 1 << 15;

    private static final int NO_POINTER = -1;
    private static final int POINTER_ID_COUNT = 32;
    private static final float MOVE_DEAD_ZONE = .12f;
    private static final long CAMERA_IDLE_MS = 48;
    private static final float CAMERA_FULL_DEFLECTION_PER_MS = .0004f;
    private static final float CAMERA_JITTER_FRACTION = .0008f;
    private static final float CAMERA_DEAD_ZONE_WITH_MARGIN = .42f;

    private final boolean[] buttonPointers = new boolean[POINTER_ID_COUNT];
    private final int[] heldButtons = new int[POINTER_ID_COUNT];
    private int movementPointer = NO_POINTER;
    private int cameraPointer = NO_POINTER;
    private float movementCenterX;
    private float movementCenterY;
    private float movementRadius = 1;
    private float cameraTravelPerMs = 1;
    private float cameraJitter = .5f;
    private float cameraSensitivity = 1;
    private float cameraSignX = -1;
    private float cameraSignY = -1;
    private float previousCameraX;
    private float previousCameraY;
    private long previousCameraTime;

    int buttons;
    float moveX;
    float moveY;
    float cameraX;
    float cameraY;
    long cameraIdleAt;

    void configureGeometry(float movementCenterX, float movementCenterY,
                           float movementRadius, float shortSide) {
        this.movementCenterX = movementCenterX;
        this.movementCenterY = movementCenterY;
        this.movementRadius = Math.max(1, movementRadius);
        float screenSize = Math.max(1, shortSide);
        cameraTravelPerMs = screenSize * CAMERA_FULL_DEFLECTION_PER_MS;
        cameraJitter = Math.max(.5f, screenSize * CAMERA_JITTER_FRACTION);
    }

    void configureCamera(float sensitivity, boolean invertX, boolean invertY) {
        cameraSensitivity = sensitivity;
        cameraSignX = invertX ? 1 : -1;
        cameraSignY = invertY ? 1 : -1;
        clearCameraAxes();
    }

    void pointerDown(int id, float x, float y, long time, int button,
                     boolean onMovement, boolean onCamera) {
        if (!validId(id)) {
            return;
        }
        if (button != 0) {
            buttonPointers[id] = true;
            setHeldButton(id, button);
        } else if (onMovement) {
            if (movementPointer == NO_POINTER) {
                movementPointer = id;
                updateMovement(x, y);
            }
        } else if (onCamera && cameraPointer == NO_POINTER) {
            cameraPointer = id;
            previousCameraX = x;
            previousCameraY = y;
            previousCameraTime = time;
        } else {
            buttonPointers[id] = true;
        }
    }

    void pointerMove(int id, float x, float y, long time, int button) {
        if (!validId(id)) {
            return;
        }
        if (id == movementPointer) {
            updateMovement(x, y);
        } else if (id == cameraPointer) {
            updateCamera(x, y, time);
        } else if (buttonPointers[id]) {
            setHeldButton(id, button);
        }
    }

    void pointerUp(int id) {
        if (!validId(id)) {
            return;
        }
        if (id == movementPointer) {
            movementPointer = NO_POINTER;
            moveX = 0;
            moveY = 0;
        }
        if (id == cameraPointer) {
            cameraPointer = NO_POINTER;
            clearCameraAxes();
        }
        buttonPointers[id] = false;
        setHeldButton(id, 0);
    }

    void expireCamera(long now) {
        if (cameraIdleAt != 0 && now >= cameraIdleAt) {
            clearCameraAxes();
        }
    }

    void reset() {
        Arrays.fill(buttonPointers, false);
        Arrays.fill(heldButtons, 0);
        buttons = 0;
        movementPointer = NO_POINTER;
        moveX = 0;
        moveY = 0;
        cameraPointer = NO_POINTER;
        clearCameraAxes();
    }

    private void updateMovement(float x, float y) {
        float dx = (x - movementCenterX) / movementRadius;
        float dy = (y - movementCenterY) / movementRadius;
        float distance = (float) Math.sqrt(dx * dx + dy * dy);
        if (distance <= MOVE_DEAD_ZONE) {
            moveX = 0;
            moveY = 0;
            return;
        }
        float response = (Math.min(1, distance) - MOVE_DEAD_ZONE) / (1 - MOVE_DEAD_ZONE);
        moveX = dx * response / distance;
        moveY = dy * response / distance;
    }

    private void updateCamera(float x, float y, long time) {
        float dx = x - previousCameraX;
        float dy = y - previousCameraY;
        float distance = (float) Math.hypot(dx, dy);
        if (distance < cameraJitter) {
            return;
        }
        long elapsedMs = Math.max(4, Math.min(CAMERA_IDLE_MS, time - previousCameraTime));
        float speed = Math.min(1, distance * cameraSensitivity / (cameraTravelPerMs * elapsedMs));
        // The game subtracts a radial dead zone of .4 from its right stick.
        // Boost deliberate slow swipes while leaving room for the pad's 8-bit axes.
        float response = speed * (2 - speed);
        float deflection = CAMERA_DEAD_ZONE_WITH_MARGIN + (1 - CAMERA_DEAD_ZONE_WITH_MARGIN) * response;
        cameraX = (dx / distance) * deflection * cameraSignX;
        cameraY = (dy / distance) * deflection * cameraSignY;
        previousCameraX = x;
        previousCameraY = y;
        previousCameraTime = time;
        cameraIdleAt = time + CAMERA_IDLE_MS;
    }

    private void clearCameraAxes() {
        cameraX = 0;
        cameraY = 0;
        cameraIdleAt = 0;
    }

    private void setHeldButton(int id, int button) {
        if (heldButtons[id] == button) {
            return;
        }
        heldButtons[id] = button;
        buttons = 0;
        for (int held : heldButtons) {
            buttons |= held;
        }
    }

    private static boolean validId(int id) {
        return id >= 0 && id < POINTER_ID_COUNT;
    }
}
