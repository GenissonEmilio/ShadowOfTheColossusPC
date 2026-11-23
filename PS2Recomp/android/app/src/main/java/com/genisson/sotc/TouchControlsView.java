package com.genisson.sotc;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Build;
import android.os.SystemClock;
import android.view.DisplayCutout;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

public final class TouchControlsView extends View {
    public interface Listener {
        void onInput(int buttons, float moveX, float moveY, float cameraX, float cameraY);
        void onMenu();
    }

    private static final int STONE = 0xe51b1a16;
    private static final int SHADOW = 0xd015130f;
    private static final int BRONZE = 0xffb48f56;
    private static final int GOLD = 0xffffcf78;
    private static final int IVORY = 0xfff6e3be;
    private static final int RIM = 0xff504232;
    private static final int BRONZE_FILL = 0xed74512a;
    private static final float SWORD_TIP_Y = -.40f;
    private static final float SWORD_GLOW_RADIUS = .32f;
    private static final float DEFAULT_SCALE = 1;
    private static final float DEFAULT_OPACITY = .73f;
    private static final float DEFAULT_CAMERA_SENSITIVITY = 1;

    private final TouchInputState input = new TouchInputState();
    private final SharedPreferences preferences;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final Shader swordGlow = new RadialGradient(0, SWORD_TIP_Y, SWORD_GLOW_RADIUS,
            0xeeffbd4f, 0x00ffbd4f, Shader.TileMode.CLAMP);
    private final Typeface labelTypeface = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    private final Control movement = new Control("movement", "", 0, .225f, .71f, .153f);
    private final Control menu = new Control("menu", "", 0, -.088f, .086f, .059f);
    private final Control[] controls = {
        movement,
        new Control("focus", "Focar", TouchInputState.L1, .145f, .36f, .070f),
        new Control("weapon-left", "Arma", TouchInputState.LEFT, .405f, .885f, .065f),
        new Control("weapon-right", "Arma", TouchInputState.RIGHT, .592f, .885f, .065f),
        new Control("grip", "Agarrar", TouchInputState.R1, -.177f, .473f, .081f),
        new Control("jump", "Pular", TouchInputState.TRIANGLE, -.291f, .893f, .078f),
        new Control("attack", "Atacar", TouchInputState.SQUARE, -.205f, .701f, .076f),
        new Control("action", "Ação", TouchInputState.CIRCLE, -.392f, .619f, .076f),
        new Control("agro", "Agro", TouchInputState.CROSS, -.511f, .819f, .076f),
        new Control("map", "Mapa", TouchInputState.SELECT, -.095f, .091f, .060f),
        new Control("pause", "Pausa", TouchInputState.START, .095f, .091f, .060f)
    };

    private final Runnable cameraIdle = this::onCameraIdle;
    private Listener listener;
    private float controlScale;
    private float opacity;
    private float cameraSensitivity;
    private boolean invertCameraX;
    private boolean invertCameraY;
    private boolean editing;
    private boolean cameraIdlePosted;
    private boolean touchSequence;
    private int menuPointer = -1;
    private int dragPointer = -1;
    private Control dragControl;
    private float dragOffsetX;
    private float dragOffsetY;
    private int cutoutLeft;
    private int cutoutTop;
    private int cutoutRight;
    private int cutoutBottom;
    private float safeLeft;
    private float safeTop;
    private float safeRight;
    private float safeBottom;
    private float shortSide;
    private int drawnButtons;
    private float drawnMoveX;
    private float drawnMoveY;
    private float sentCameraX;
    private float sentCameraY;

    public TouchControlsView(Context context) {
        this(context, context.getSharedPreferences("sotc-touch-controls", Context.MODE_PRIVATE));
    }

    TouchControlsView(Context context, SharedPreferences preferences) {
        super(context);
        this.preferences = preferences;
        controlScale = clamp(preferences.getFloat("scale", DEFAULT_SCALE), .7f, 1.3f, DEFAULT_SCALE);
        opacity = clamp(preferences.getFloat("opacity", DEFAULT_OPACITY), .2f, 1, DEFAULT_OPACITY);
        cameraSensitivity = clamp(preferences.getFloat("sensitivity", DEFAULT_CAMERA_SENSITIVITY),
                .35f, 2.5f, DEFAULT_CAMERA_SENSITIVITY);
        invertCameraX = preferences.getBoolean("invert-camera-x", false);
        invertCameraY = preferences.getBoolean("invert-camera-y", false);
        input.configureCamera(cameraSensitivity, invertCameraX, invertCameraY);
        for (Control control : controls) {
            control.savedX = clamp(preferences.getFloat(control.key + "-x", Float.NaN), 0, 1, Float.NaN);
            control.savedY = clamp(preferences.getFloat(control.key + "-y", Float.NaN), 0, 1, Float.NaN);
        }
        setClickable(true);
        setContentDescription("Ajustes dos controles de toque");
    }

    public void setListener(Listener listener) {
        if (this.listener == listener) {
            return;
        }
        releaseAll();
        this.listener = listener;
        if (listener != null) {
            listener.onInput(0, 0, 0, 0, 0);
        }
    }

    public void setEditing(boolean editing) {
        if (this.editing != editing) {
            releaseAll();
            this.editing = editing;
            setContentDescription(editing ? "Concluir edição dos controles" : "Ajustes dos controles de toque");
            invalidate();
        }
    }

    public boolean isEditing() {
        return editing;
    }

    public float getControlScale() {
        return controlScale;
    }

    public void setControlScale(float scale) {
        controlScale = clamp(scale, .7f, 1.3f, DEFAULT_SCALE);
        preferences.edit().putFloat("scale", controlScale).apply();
        releaseAll();
        layoutControls();
    }

    public float getOpacity() {
        return opacity;
    }

    public void setOpacity(float opacity) {
        this.opacity = clamp(opacity, .2f, 1, DEFAULT_OPACITY);
        preferences.edit().putFloat("opacity", this.opacity).apply();
        invalidate();
    }

    public float getCameraSensitivity() {
        return cameraSensitivity;
    }

    public void setCameraSensitivity(float sensitivity) {
        cameraSensitivity = clamp(sensitivity, .35f, 2.5f, DEFAULT_CAMERA_SENSITIVITY);
        preferences.edit().putFloat("sensitivity", cameraSensitivity).apply();
        applyCameraSettings();
    }

    public boolean isCameraXInverted() {
        return invertCameraX;
    }

    public boolean isCameraYInverted() {
        return invertCameraY;
    }

    public void setInvertCamera(boolean invertX, boolean invertY) {
        invertCameraX = invertX;
        invertCameraY = invertY;
        preferences.edit().putBoolean("invert-camera-x", invertX)
                .putBoolean("invert-camera-y", invertY).apply();
        applyCameraSettings();
    }

    public void resetLayout() {
        releaseAll();
        clearSavedLayout(preferences.edit());
    }

    public void resetSettings() {
        releaseAll();
        controlScale = DEFAULT_SCALE;
        opacity = DEFAULT_OPACITY;
        cameraSensitivity = DEFAULT_CAMERA_SENSITIVITY;
        invertCameraX = false;
        invertCameraY = false;
        applyCameraSettings();
        clearSavedLayout(preferences.edit().clear());
    }

    private void clearSavedLayout(SharedPreferences.Editor changes) {
        for (Control control : controls) {
            control.savedX = Float.NaN;
            control.savedY = Float.NaN;
            changes.remove(control.key + "-x").remove(control.key + "-y");
        }
        changes.apply();
        layoutControls();
    }

    public void releaseAll() {
        input.reset();
        touchSequence = false;
        menuPointer = -1;
        endDrag();
        dispatchInput();
    }

    private void applyCameraSettings() {
        input.configureCamera(cameraSensitivity, invertCameraX, invertCameraY);
        dispatchInput();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        requestApplyInsets();
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (!hasWindowFocus) {
            releaseAll();
        }
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        DisplayCutout cutout = insets.getDisplayCutout();
        int left = cutout == null ? 0 : cutout.getSafeInsetLeft();
        int top = cutout == null ? 0 : cutout.getSafeInsetTop();
        int right = cutout == null ? 0 : cutout.getSafeInsetRight();
        int bottom = cutout == null ? 0 : cutout.getSafeInsetBottom();
        if (cutout != null && Build.VERSION.SDK_INT >= 30) {
            Insets waterfall = cutout.getWaterfallInsets();
            left = Math.max(left, waterfall.left);
            top = Math.max(top, waterfall.top);
            right = Math.max(right, waterfall.right);
            bottom = Math.max(bottom, waterfall.bottom);
        }
        if (left != cutoutLeft || top != cutoutTop || right != cutoutRight || bottom != cutoutBottom) {
            releaseAll();
            cutoutLeft = left;
            cutoutTop = top;
            cutoutRight = right;
            cutoutBottom = bottom;
            layoutControls();
        }
        return super.onApplyWindowInsets(insets);
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        releaseAll();
        layoutControls();
    }

    private void layoutControls() {
        safeLeft = cutoutLeft;
        safeTop = cutoutTop;
        safeRight = Math.max(safeLeft, getWidth() - cutoutRight);
        safeBottom = Math.max(safeTop, getHeight() - cutoutBottom);
        shortSide = Math.min(safeRight - safeLeft, safeBottom - safeTop);
        for (Control control : controls) {
            layoutControl(control, controlScale);
        }
        layoutControl(menu, 1);
        input.configureGeometry(movement.x, movement.y, .67f * movement.radius, shortSide);
        invalidate();
    }

    private void layoutControl(Control control, float scale) {
        float x = Float.isNaN(control.savedX) ? defaultX(control)
                : safeLeft + control.savedX * (safeRight - safeLeft);
        float normalizedY = Float.isNaN(control.savedY) ? control.anchorY : control.savedY;
        float y = safeTop + normalizedY * (safeBottom - safeTop);
        control.radius = shortSide * control.baseRadius * scale;
        positionControl(control, x, y);
    }

    private float defaultX(Control control) {
        if (control.button == TouchInputState.SELECT || control.button == TouchInputState.START) {
            return (safeLeft + safeRight) / 2 + shortSide * control.anchorX;
        }
        float edge = control.anchorX < 0 ? safeRight : safeLeft;
        return edge + shortSide * control.anchorX;
    }

    private void positionControl(Control control, float x, float y) {
        float margin = control.radius + shortSide * .012f;
        control.x = clamp(x, safeLeft + margin, safeRight - margin, (safeLeft + safeRight) / 2);
        control.y = clamp(y, safeTop + margin, safeBottom - margin, (safeTop + safeBottom) / 2);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            releaseAll();
            touchSequence = true;
        } else if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        if (!touchSequence || shortSide == 0) {
            return true;
        }
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                onPointerDown(event, index);
                break;
            case MotionEvent.ACTION_MOVE:
                onPointerMove(event);
                break;
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
                onPointerUp(event, index);
                if (action == MotionEvent.ACTION_UP) {
                    releaseAll();
                }
                break;
            default:
                break;
        }
        input.expireCamera(SystemClock.uptimeMillis());
        dispatchInput();
        return true;
    }

    private void onPointerDown(MotionEvent event, int index) {
        int id = event.getPointerId(index);
        float x = event.getX(index);
        float y = event.getY(index);
        if (hit(menu, x, y)) {
            if (menuPointer == -1) {
                menuPointer = id;
                invalidate();
            }
            return;
        }
        Control control = controlAt(x, y);
        if (editing) {
            if (dragPointer == -1 && control != null) {
                dragPointer = id;
                dragControl = control;
                dragOffsetX = control.x - x;
                dragOffsetY = control.y - y;
                invalidate();
            }
        } else {
            input.pointerDown(id, x, y, event.getEventTime(), buttonOf(control),
                    control == movement, isCameraArea(x, y, control));
        }
    }

    private void onPointerMove(MotionEvent event) {
        if (editing) {
            if (dragControl != null) {
                int index = event.findPointerIndex(dragPointer);
                if (index >= 0) {
                    positionControl(dragControl, event.getX(index) + dragOffsetX,
                            event.getY(index) + dragOffsetY);
                    postInvalidateOnAnimation();
                }
            }
            return;
        }
        for (int index = 0; index < event.getPointerCount(); index++) {
            int id = event.getPointerId(index);
            if (id == menuPointer) {
                continue;
            }
            float x = event.getX(index);
            float y = event.getY(index);
            Control control = controlAt(x, y);
            input.pointerMove(id, x, y, event.getEventTime(), buttonOf(control), isCameraArea(x, y, control));
        }
    }

    private void onPointerUp(MotionEvent event, int index) {
        int id = event.getPointerId(index);
        if (id == menuPointer) {
            menuPointer = -1;
            invalidate();
            if (hit(menu, event.getX(index), event.getY(index))) {
                performClick();
            }
        } else if (id == dragPointer) {
            positionControl(dragControl, event.getX(index) + dragOffsetX, event.getY(index) + dragOffsetY);
            endDrag();
        } else {
            input.pointerUp(id);
        }
    }

    @Override
    public boolean performClick() {
        super.performClick();
        releaseAll();
        if (listener != null) {
            listener.onMenu();
        }
        return true;
    }

    private void endDrag() {
        if (dragControl == null) {
            return;
        }
        float width = safeRight - safeLeft;
        float height = safeBottom - safeTop;
        if (width > 0 && height > 0) {
            dragControl.savedX = (dragControl.x - safeLeft) / width;
            dragControl.savedY = (dragControl.y - safeTop) / height;
            preferences.edit().putFloat(dragControl.key + "-x", dragControl.savedX)
                    .putFloat(dragControl.key + "-y", dragControl.savedY).apply();
        }
        dragPointer = -1;
        dragControl = null;
        input.configureGeometry(movement.x, movement.y, .67f * movement.radius, shortSide);
        invalidate();
    }

    private Control controlAt(float x, float y) {
        Control closest = null;
        float bestDistance = Float.POSITIVE_INFINITY;
        for (Control control : controls) {
            if (hit(control, x, y)) {
                float distance = normalizedDistanceSquared(control, x, y);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    closest = control;
                }
            }
        }
        return closest;
    }

    private boolean isCameraArea(float x, float y, Control control) {
        return control == null && x >= safeLeft + .46f * (safeRight - safeLeft) && x <= safeRight
                && y >= safeTop && y <= safeBottom && !hit(menu, x, y);
    }

    private boolean hit(Control control, float x, float y) {
        float radius = control.radius + shortSide * .009f;
        float dx = x - control.x;
        float dy = y - control.y;
        return dx * dx + dy * dy <= radius * radius;
    }

    private static float normalizedDistanceSquared(Control control, float x, float y) {
        float dx = (x - control.x) / control.radius;
        float dy = (y - control.y) / control.radius;
        return dx * dx + dy * dy;
    }

    private static int buttonOf(Control control) {
        return control == null ? 0 : control.button;
    }

    // A zero button identifies the left analog control.
    RectF controlBounds(int button) {
        for (Control control : controls) {
            if (control.button == button) {
                return new RectF(control.x - control.radius, control.y - control.radius,
                        control.x + control.radius, control.y + control.radius);
            }
        }
        throw new IllegalArgumentException("Unknown touch control: " + button);
    }

    private void onCameraIdle() {
        cameraIdlePosted = false;
        input.expireCamera(SystemClock.uptimeMillis());
        dispatchInput();
    }

    private void dispatchInput() {
        boolean redraw = input.buttons != drawnButtons || input.moveX != drawnMoveX || input.moveY != drawnMoveY;
        if (redraw || input.cameraX != sentCameraX || input.cameraY != sentCameraY) {
            drawnButtons = input.buttons;
            drawnMoveX = input.moveX;
            drawnMoveY = input.moveY;
            sentCameraX = input.cameraX;
            sentCameraY = input.cameraY;
            if (listener != null) {
                listener.onInput(drawnButtons, drawnMoveX, drawnMoveY, sentCameraX, sentCameraY);
            }
            if (redraw) {
                postInvalidateOnAnimation();
            }
        }
        if (input.cameraIdleAt == 0) {
            if (cameraIdlePosted) {
                removeCallbacks(cameraIdle);
                cameraIdlePosted = false;
            }
        } else if (!cameraIdlePosted) {
            long delay = Math.max(1, input.cameraIdleAt - SystemClock.uptimeMillis());
            cameraIdlePosted = postDelayed(cameraIdle, delay);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (shortSide == 0) {
            return;
        }
        if (editing) {
            drawEditor(canvas);
        }
        for (Control control : controls) {
            if (control == movement) {
                drawMovement(canvas);
            } else {
                drawButton(canvas, control, (drawnButtons & control.button) != 0);
            }
        }
        drawButton(canvas, menu, menuPointer != -1 || editing);
    }

    private void drawEditor(Canvas canvas) {
        fill(0x7d19140f, 1);
        canvas.drawRect(safeLeft, safeTop, safeRight, safeBottom, paint);
        paint.setTypeface(labelTypeface);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(shortSide * .034f);
        fill(IVORY, 1);
        float x = (safeLeft + safeRight) / 2;
        float y = safeTop + (safeBottom - safeTop) * .49f;
        canvas.drawText("Arraste os controles para mover", x, y, paint);
        paint.setTextSize(shortSide * .029f);
        fill(BRONZE, 1);
        canvas.drawText("Toque no sigilo para concluir", x, y + shortSide * .045f, paint);
    }

    private void drawMovement(Canvas canvas) {
        canvas.save();
        canvas.translate(movement.x, movement.y);
        canvas.scale(movement.radius, movement.radius);
        float visibility = editing ? 1 : opacity;
        drawStoneRim(canvas, visibility, dragControl == movement);
        stroke(BRONZE, visibility * .65f, .021f);
        canvas.drawCircle(0, 0, .64f, paint);
        for (int quarter = 0; quarter < 4; quarter++) {
            canvas.drawLine(0, -.92f, 0, -.78f, paint);
            canvas.rotate(90);
        }

        float x = drawnMoveX * .67f;
        float y = drawnMoveY * .67f;
        fill(drawnMoveX == 0 && drawnMoveY == 0 ? RIM : BRONZE_FILL, visibility);
        canvas.drawCircle(x, y, .29f, paint);
        stroke(BRONZE, visibility, .035f);
        canvas.drawCircle(x, y, .29f, paint);
        stroke(IVORY, visibility, .028f);
        path.rewind();
        path.moveTo(x, y - .11f);
        path.lineTo(x + .085f, y);
        path.lineTo(x, y + .11f);
        path.lineTo(x - .085f, y);
        path.close();
        canvas.drawPath(path, paint);
        canvas.restore();
    }

    private void drawButton(Canvas canvas, Control control, boolean pressed) {
        canvas.save();
        canvas.translate(control.x, control.y);
        canvas.scale(control.radius, control.radius);
        float visibility = editing ? 1 : opacity;
        drawStoneRim(canvas, visibility, pressed || dragControl == control);
        if (control == menu) {
            stroke(IVORY, visibility, .052f);
            drawSigil(canvas);
        } else {
            canvas.translate(0, -.20f);
            drawButtonSymbol(canvas, control.button, pressed ? GOLD : IVORY, visibility);
        }
        canvas.restore();
        if (control != menu) {
            // Text at a fractional (< 1 px) paint size loses glyph advances on
            // Android even when the canvas is enlarged. Rasterize at pixel size.
            paint.setTypeface(labelTypeface);
            paint.setTextAlign(Paint.Align.CENTER);
            fill(IVORY, visibility);
            if (control.button == TouchInputState.L1) {
                paint.setTextSize(control.radius * .54f);
                canvas.drawText("L1", control.x, control.y - control.radius * .03f, paint);
            }
            paint.setTextSize(control.radius * .365f);
            canvas.drawText(control.label, control.x, control.y + control.radius * .60f, paint);
        }
    }

    private void drawStoneRim(Canvas canvas, float visibility, boolean highlight) {
        fill(SHADOW, visibility);
        canvas.drawCircle(0, .055f, 1, paint);
        fill(highlight ? BRONZE_FILL : STONE, visibility);
        canvas.drawCircle(0, 0, .980f, paint);
        stroke(highlight ? GOLD : BRONZE, visibility, .045f);
        canvas.drawCircle(0, 0, .956f, paint);
        stroke(highlight ? BRONZE : RIM, visibility, .024f);
        canvas.drawCircle(0, 0, .861f, paint);
        stroke(highlight ? GOLD : BRONZE, visibility * .7f, .038f);
        canvas.drawArc(-.861f, -.861f, .861f, .861f, 198, 92, false, paint);
        canvas.drawArc(-.861f, -.861f, .861f, .861f, 18, 92, false, paint);
    }

    private void drawButtonSymbol(Canvas canvas, int mask, int color, float visibility) {
        stroke(color, visibility, .058f);
        switch (mask) {
            case TouchInputState.TRIANGLE:
                drawBoot(canvas);
                break;
            case TouchInputState.SQUARE:
                drawSword(canvas, color, visibility, false);
                break;
            case TouchInputState.CIRCLE:
                drawSword(canvas, color, visibility, true);
                break;
            case TouchInputState.CROSS:
                drawHorseHead(canvas, color, visibility);
                break;
            case TouchInputState.R1:
                drawGrippingHand(canvas);
                break;
            case TouchInputState.LEFT:
            case TouchInputState.RIGHT:
                float direction = mask == TouchInputState.LEFT ? -1 : 1;
                path.rewind();
                path.moveTo(-direction * .08f, -.26f);
                path.lineTo(direction * .21f, 0);
                path.lineTo(-direction * .08f, .26f);
                canvas.drawPath(path, paint);
                break;
            case TouchInputState.START:
                canvas.drawLine(-.13f, -.28f, -.13f, .22f, paint);
                canvas.drawLine(.13f, -.28f, .13f, .22f, paint);
                break;
            case TouchInputState.SELECT:
                path.rewind();
                path.moveTo(-.36f, -.18f);
                path.lineTo(-.12f, -.28f);
                path.lineTo(.12f, -.18f);
                path.lineTo(.36f, -.28f);
                path.lineTo(.36f, .15f);
                path.lineTo(.12f, .25f);
                path.lineTo(-.12f, .15f);
                path.lineTo(-.36f, .25f);
                path.close();
                canvas.drawPath(path, paint);
                canvas.drawLine(-.12f, -.28f, -.12f, .15f, paint);
                canvas.drawLine(.12f, -.18f, .12f, .25f, paint);
                break;
            default:
                break;
        }
    }

    private void drawHorseHead(Canvas canvas, int color, float visibility) {
        path.rewind();
        path.moveTo(-.36f, .28f);
        path.cubicTo(-.34f, .07f, -.32f, -.20f, -.18f, -.27f);
        path.lineTo(-.17f, -.43f);
        path.lineTo(-.03f, -.31f);
        path.lineTo(.06f, -.41f);
        path.lineTo(.10f, -.25f);
        path.quadTo(.17f, -.19f, .20f, -.09f);
        path.lineTo(.35f, .08f);
        path.quadTo(.42f, .17f, .30f, .22f);
        path.quadTo(.24f, .25f, .17f, .20f);
        path.lineTo(.02f, .10f);
        path.quadTo(-.04f, .18f, -.01f, .28f);
        path.close();
        path.moveTo(-.15f, -.19f);
        path.quadTo(-.24f, -.08f, -.24f, .15f);
        canvas.drawPath(path, paint);
        fill(color, visibility);
        canvas.drawCircle(.07f, -.12f, .042f, paint);
    }

    private void drawSword(Canvas canvas, int color, float visibility, boolean lit) {
        canvas.save();
        canvas.rotate(43);
        if (lit) {
            fill(GOLD, visibility);
            paint.setShader(swordGlow);
            canvas.drawCircle(0, SWORD_TIP_Y, SWORD_GLOW_RADIUS, paint);
            paint.setShader(null);
        }
        stroke(color, visibility, .058f);
        path.rewind();
        path.moveTo(0, SWORD_TIP_Y);
        path.lineTo(.096f, -.25f);
        path.lineTo(.080f, .12f);
        path.lineTo(-.080f, .12f);
        path.lineTo(-.096f, -.25f);
        path.close();
        canvas.drawPath(path, paint);
        canvas.drawLine(-.18f, .12f, .18f, .12f, paint);
        canvas.drawLine(0, .12f, 0, .34f, paint);
        canvas.drawLine(-.057f, .34f, .057f, .34f, paint);
        if (lit) {
            fill(GOLD, visibility);
            path.rewind();
            path.moveTo(0, SWORD_TIP_Y - .24f);
            path.lineTo(.045f, SWORD_TIP_Y - .047f);
            path.lineTo(.17f, SWORD_TIP_Y);
            path.lineTo(.045f, SWORD_TIP_Y + .047f);
            path.lineTo(0, SWORD_TIP_Y + .19f);
            path.lineTo(-.045f, SWORD_TIP_Y + .047f);
            path.lineTo(-.17f, SWORD_TIP_Y);
            path.lineTo(-.045f, SWORD_TIP_Y - .047f);
            path.close();
            canvas.drawPath(path, paint);
        }
        canvas.restore();
    }

    private void drawBoot(Canvas canvas) {
        path.rewind();
        path.moveTo(-.28f, -.34f);
        path.lineTo(.02f, -.34f);
        path.lineTo(.04f, -.09f);
        path.quadTo(.06f, 0, .19f, .028f);
        path.quadTo(.39f, .043f, .39f, .18f);
        path.lineTo(.39f, .26f);
        path.lineTo(-.08f, .26f);
        path.lineTo(-.12f, .22f);
        path.lineTo(-.16f, .26f);
        path.lineTo(-.33f, .26f);
        path.lineTo(-.33f, .12f);
        path.quadTo(-.27f, -.10f, -.28f, -.34f);
        path.close();
        canvas.drawPath(path, paint);
        canvas.drawLine(-.29f, .15f, .38f, .15f, paint);
        canvas.drawLine(-.11f, -.17f, .03f, -.17f, paint);
        canvas.drawLine(-.09f, -.06f, .05f, -.06f, paint);
    }

    private void drawGrippingHand(Canvas canvas) {
        path.rewind();
        path.moveTo(-.14f, .32f);
        path.lineTo(-.15f, .22f);
        path.quadTo(-.31f, .13f, -.34f, -.06f);
        path.quadTo(-.36f, -.17f, -.29f, -.21f);
        path.quadTo(-.24f, -.24f, -.19f, -.19f);
        path.quadTo(-.18f, -.33f, -.08f, -.35f);
        path.quadTo(0, -.36f, .03f, -.31f);
        path.quadTo(.12f, -.37f, .18f, -.28f);
        path.quadTo(.28f, -.31f, .31f, -.21f);
        path.quadTo(.39f, -.22f, .39f, -.10f);
        path.quadTo(.37f, .11f, .24f, .22f);
        path.lineTo(.23f, .32f);
        path.close();
        path.moveTo(-.19f, -.19f);
        path.lineTo(-.08f, -.10f);
        path.quadTo(.05f, -.13f, .08f, -.06f);
        path.quadTo(.11f, .025f, -.05f, .05f);
        path.moveTo(.03f, -.31f);
        path.lineTo(.06f, -.18f);
        path.moveTo(.18f, -.28f);
        path.lineTo(.20f, -.13f);
        path.moveTo(.31f, -.21f);
        path.lineTo(.32f, -.10f);
        canvas.drawPath(path, paint);
    }

    private void drawSigil(Canvas canvas) {
        path.rewind();
        path.moveTo(0, -.58f);
        path.lineTo(.52f, 0);
        path.lineTo(0, .58f);
        path.lineTo(-.52f, 0);
        path.close();
        path.moveTo(-.30f, .03f);
        path.lineTo(-.09f, -.19f);
        path.lineTo(.10f, .11f);
        path.lineTo(.29f, -.08f);
        canvas.drawPath(path, paint);
    }

    private void fill(int color, float opacity) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(color);
        paint.setAlpha(Math.round((color >>> 24) * opacity));
    }

    private void stroke(int color, float opacity, float lineWidth) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(color);
        paint.setAlpha(Math.round((color >>> 24) * opacity));
        paint.setStrokeWidth(lineWidth);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    private static float clamp(float value, float min, float max, float fallback) {
        return Float.isFinite(value) && min <= max ? Math.max(min, Math.min(max, value)) : fallback;
    }

    private static final class Control {
        final String key;
        final String label;
        final int button;
        final float anchorX;
        final float anchorY;
        final float baseRadius;
        float savedX = Float.NaN;
        float savedY = Float.NaN;
        float x;
        float y;
        float radius;

        Control(String key, String label, int button, float anchorX, float anchorY, float baseRadius) {
            this.key = key;
            this.label = label;
            this.button = button;
            this.anchorX = anchorX;
            this.anchorY = anchorY;
            this.baseRadius = baseRadius;
        }
    }
}
