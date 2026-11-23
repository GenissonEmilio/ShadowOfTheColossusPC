package com.genisson.sotc;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.RelativeSizeSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

final class LauncherScreen extends ScrollView {
    static final int BACKGROUND = 0xff161411;
    static final int GOLD = 0xffddbf83;
    private static final int CREAM = 0xfff5eada;
    private static final int MUTED = 0xffc1b4a0;
    private static final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT;
    private static final int WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;

    final Button playButton;
    final Button importButton;
    final Button controlsButton;
    final TextView status;
    final ProgressBar progress;

    private final LinearLayout page;
    private final FrameLayout hero;
    private final LinearLayout titleBlock;
    private final TextView title;
    private final LinearLayout menuColumn;
    private final LinearLayout menu;
    private final LinearLayout card;
    private Boolean importHighlighted;

    LauncherScreen(Context context) {
        super(context);
        setFillViewport(true);
        setBackgroundColor(BACKGROUND);
        setVerticalScrollBarEnabled(false);
        setOverScrollMode(OVER_SCROLL_IF_CONTENT_SCROLLS);

        page = vertical();
        addView(page, new ScrollView.LayoutParams(MATCH, WRAP));

        hero = new FrameLayout(context);
        hero.setBackground(new HeroArt(BitmapFactory.decodeResource(
                getResources(), R.drawable.launcher_landscape)));
        page.addView(hero);

        titleBlock = vertical();
        titleBlock.setGravity(Gravity.CENTER_HORIZONTAL);
        hero.addView(titleBlock, new FrameLayout.LayoutParams(MATCH, WRAP, Gravity.TOP));

        ImageView sigil = new ImageView(context);
        sigil.setImageResource(R.drawable.ic_launcher_sigil);
        sigil.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        titleBlock.addView(sigil, new LinearLayout.LayoutParams(dp(34), dp(34)));

        SpannableString wordmark = new SpannableString(getResources().getText(R.string.launcher_title));
        int subtitleStart = wordmark.toString().indexOf('\n') + 1;
        wordmark.setSpan(new RelativeSizeSpan(.53f), subtitleStart, wordmark.length(),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        title = text(wordmark, 48, CREAM);
        title.setTypeface(Typeface.create("serif", Typeface.NORMAL));
        title.setLetterSpacing(.085f);
        title.setGravity(Gravity.CENTER);
        title.setAccessibilityHeading(true);
        title.setShadowLayer(dp(6), 0, dp(2), 0xe6000000);
        add(titleBlock, title, 8);

        menuColumn = vertical();
        menuColumn.setGravity(Gravity.CENTER);
        page.addView(menuColumn);
        menu = vertical();
        menuColumn.addView(menu);

        card = vertical();
        card.setBackground(surface(0xff242019, 0xff1d1a16, 0xff554633));
        add(menu, card, 0);

        LinearLayout discHeading = new LinearLayout(context);
        discHeading.setGravity(Gravity.CENTER_VERTICAL);
        add(card, discHeading, 0);
        TextView heading = text(getResources().getText(R.string.launcher_disc_heading), 16, GOLD);
        heading.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        heading.setAccessibilityHeading(true);
        discHeading.addView(heading, new LinearLayout.LayoutParams(0, WRAP, 1f));

        progress = new ProgressBar(context, null, android.R.attr.progressBarStyleSmall);
        progress.setIndeterminateTintList(ColorStateList.valueOf(GOLD));
        progress.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        progress.setVisibility(GONE);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(dp(20), dp(20));
        progressParams.setMarginStart(dp(10));
        discHeading.addView(progress, progressParams);

        status = text("", 14, MUTED);
        status.setAccessibilityLiveRegion(ACCESSIBILITY_LIVE_REGION_POLITE);
        add(card, status, 4);

        playButton = action(R.string.launcher_play, R.drawable.ic_launcher_sword);
        add(card, playButton, 14);
        importButton = action(R.string.launcher_import, R.drawable.ic_launcher_disc);
        add(card, importButton, 8);
        controlsButton = action(R.string.launcher_controls, R.drawable.ic_launcher_controls);
        styleAction(controlsButton, false);
        add(card, controlsButton, 8);
        setImportHighlighted(false);

        String footnote = getResources().getString(R.string.launcher_disc_edition) + "\n"
                + getResources().getString(R.string.launcher_saves);
        TextView footer = text(footnote, 12, MUTED);
        footer.setGravity(Gravity.CENTER);
        footer.setLineSpacing(dp(3), 1f);
        add(menu, footer, 12);

        updateLayout(getResources().getConfiguration());
        setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                Insets safe = insets.getInsets(
                        WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                setPadding(safe.left, safe.top, safe.right, safe.bottom);
            }
            return insets;
        });
    }

    void updateLayout(Configuration config) {
        boolean landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE
                && config.screenWidthDp >= 640;
        page.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        page.setGravity(landscape ? Gravity.CENTER_VERTICAL : Gravity.TOP);
        if (landscape) {
            hero.setMinimumHeight(dp(268));
            hero.setLayoutParams(new LinearLayout.LayoutParams(0, MATCH, 1f));
            menuColumn.setLayoutParams(new LinearLayout.LayoutParams(
                    dp(Math.min(467, Math.round(config.screenWidthDp * .46f))), WRAP));
        } else {
            int height = Math.max(260, Math.min(450, Math.round(config.screenHeightDp * .46f)));
            hero.setMinimumHeight(dp(height));
            hero.setLayoutParams(new LinearLayout.LayoutParams(MATCH, WRAP, 1f));
            menuColumn.setLayoutParams(new LinearLayout.LayoutParams(MATCH, WRAP));
        }
        int margin = config.screenWidthDp < 340 ? 14 : 22;
        menuColumn.setPadding(dp(margin), dp(landscape ? 12 : 0), dp(margin), dp(landscape ? 12 : 20));
        boolean widePortrait = !landscape && config.screenWidthDp - margin * 2 > 460;
        menu.setLayoutParams(new LinearLayout.LayoutParams(widePortrait ? dp(460) : MATCH, WRAP));
        card.setPadding(dp(16), dp(landscape ? 12 : 16), dp(16), dp(landscape ? 12 : 16));
        titleBlock.setPadding(dp(12), dp(landscape ? 22 : 26), dp(12), dp(16));
        title.setTextSize(config.screenWidthDp < 340 ? 38 : landscape ? 45 : 50);
    }

    void setImportHighlighted(boolean highlighted) {
        if (importHighlighted != null && importHighlighted == highlighted) return;
        importHighlighted = highlighted;
        styleAction(playButton, !highlighted);
        styleAction(importButton, highlighted);
    }

    private Button action(int label, int icon) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setLetterSpacing(.025f);
        button.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        button.setMinHeight(dp(50));
        button.setMinimumHeight(dp(50));
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setCompoundDrawablePadding(dp(13));
        button.setCompoundDrawablesRelativeWithIntrinsicBounds(icon, 0, 0, 0);
        button.setStateListAnimator(null);
        return button;
    }

    private void styleAction(Button button, boolean highlighted) {
        int top = highlighted ? 0xffe4c992 : 0xff302920;
        int bottom = highlighted ? 0xffc7a468 : 0xff28221c;
        int border = highlighted ? 0xffefd7a3 : 0xff68553b;
        int label = highlighted ? 0xff22190f : CREAM;
        int[][] states = { { -android.R.attr.state_enabled }, {} };
        ColorStateList labels = new ColorStateList(states, new int[] { 0xff9b8c77, label });
        StateListDrawable background = new StateListDrawable();
        background.addState(states[0], surface(0xff252019, 0xff252019, 0xff4a3b2b));
        background.addState(new int[] { android.R.attr.state_focused }, surface(top, bottom, CREAM));
        background.addState(states[1], surface(top, bottom, border));
        button.setBackground(new RippleDrawable(
                ColorStateList.valueOf(highlighted ? 0x46291e13 : 0x45ddbf83), background, null));
        button.setPaddingRelative(dp(14), dp(9), dp(14), dp(9));
        button.setTextColor(labels);
        button.setCompoundDrawableTintList(labels);
    }

    private GradientDrawable surface(int top, int bottom, int stroke) {
        GradientDrawable drawable = new GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM, new int[] { top, bottom });
        drawable.setCornerRadius(dp(10));
        drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    private LinearLayout vertical() {
        LinearLayout layout = new LinearLayout(getContext());
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private TextView text(CharSequence content, int size, int color) {
        TextView view = new TextView(getContext());
        view.setText(content);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private void add(LinearLayout parent, View view, int topMargin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(MATCH, WRAP);
        params.topMargin = dp(topMargin);
        parent.addView(view, params);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class HeroArt extends Drawable {
        private final Bitmap bitmap;
        private final RectF imageBounds = new RectF();
        private final Paint image = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint shade = new Paint();

        HeroArt(Bitmap bitmap) {
            this.bitmap = bitmap;
        }

        @Override protected void onBoundsChange(Rect bounds) {
            if (bounds.isEmpty()) return;
            float scale = Math.max((float) bounds.width() / bitmap.getWidth(),
                    (float) bounds.height() / bitmap.getHeight());
            float width = bitmap.getWidth() * scale;
            float height = bitmap.getHeight() * scale;
            // Keep both the colossus and the rider in the portrait crop.
            float left = bounds.left - (width - bounds.width()) * .92f;
            float top = bounds.top - (height - bounds.height()) * .52f;
            imageBounds.set(left, top, left + width, top + height);
            shade.setShader(new LinearGradient(0, bounds.top, 0, bounds.bottom,
                    new int[] { 0xdf161411, 0xb8161411, 0x14161411, 0x30161411, BACKGROUND },
                    new float[] { 0f, .31f, .54f, .78f, 1f }, Shader.TileMode.CLAMP));
        }

        @Override public void draw(Canvas canvas) {
            Rect bounds = getBounds();
            int saved = canvas.save();
            canvas.clipRect(bounds);
            canvas.drawBitmap(bitmap, null, imageBounds, image);
            canvas.drawRect(bounds, shade);
            canvas.restoreToCount(saved);
        }

        @Override public void setAlpha(int alpha) {
            image.setAlpha(alpha);
            shade.setAlpha(alpha);
            invalidateSelf();
        }

        @Override public void setColorFilter(ColorFilter colorFilter) {
            image.setColorFilter(colorFilter);
            invalidateSelf();
        }

        @Override public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
