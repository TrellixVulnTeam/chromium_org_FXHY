// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.content_shell;

import android.content.Context;
import android.graphics.drawable.ClipDrawable;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.TextView.OnEditorActionListener;

import org.chromium.base.CalledByNative;
import org.chromium.base.JNINamespace;
import org.chromium.content.browser.ContentView;
import org.chromium.content.browser.ContentViewClient;
import org.chromium.content.browser.ContentViewCore;
import org.chromium.content.browser.ContentViewRenderView;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.NavigationController;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.WindowAndroid;

/**
 * Container for the various UI components that make up a shell window.
 */
@JNINamespace("content")
public class Shell extends LinearLayout {

    private static final long COMPLETED_PROGRESS_TIMEOUT_MS = 200;

    private final Runnable mClearProgressRunnable = new Runnable() {
        @Override
        public void run() {
            mProgressDrawable.setLevel(0);
        }
    };

    private ContentViewCore mContentViewCore;
    private WebContents mWebContents;
    private NavigationController mNavigationController;
    private ContentViewClient mContentViewClient;
    private EditText mUrlTextView;
    private ImageButton mPrevButton;
    private ImageButton mNextButton;
    private ImageButton mStopButton;
    private ImageButton mReloadButton;

    private ClipDrawable mProgressDrawable;

    private long mNativeShell;
    private ContentViewRenderView mContentViewRenderView;
    private WindowAndroid mWindow;

    private boolean mLoading = false;

    /**
     * Constructor for inflating via XML.
     */
    public Shell(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    /**
     * Set the SurfaceView being renderered to as soon as it is available.
     */
    public void setContentViewRenderView(ContentViewRenderView contentViewRenderView) {
        FrameLayout contentViewHolder = (FrameLayout) findViewById(R.id.contentview_holder);
        if (contentViewRenderView == null) {
            if (mContentViewRenderView != null) {
                contentViewHolder.removeView(mContentViewRenderView);
            }
        } else {
            contentViewHolder.addView(contentViewRenderView,
                    new FrameLayout.LayoutParams(
                            FrameLayout.LayoutParams.MATCH_PARENT,
                            FrameLayout.LayoutParams.MATCH_PARENT));
        }
        mContentViewRenderView = contentViewRenderView;
    }

    /**
     * Initializes the Shell for use.
     *
     * @param nativeShell The pointer to the native Shell object.
     * @param window The owning window for this shell.
     * @param client The {@link ContentViewClient} to be bound to any current or new
     *               {@link ContentViewCore}s associated with this shell.
     */
    public void initialize(long nativeShell, WindowAndroid window, ContentViewClient client) {
        mNativeShell = nativeShell;
        mWindow = window;
        mContentViewClient = client;
    }

    /**
     * Closes the shell and cleans up the native instance, which will handle destroying all
     * dependencies.
     */
    public void close() {
        if (mNativeShell == 0) return;
        nativeCloseShell(mNativeShell);
    }

    @CalledByNative
    private void onNativeDestroyed() {
        mWindow = null;
        mNativeShell = 0;
        mContentViewCore.destroy();
    }

    /**
     * @return Whether the Shell has been destroyed.
     * @see #onNativeDestroyed()
     */
    public boolean isDestroyed() {
        return mNativeShell == 0;
    }

    /**
     * @return Whether or not the Shell is loading content.
     */
    public boolean isLoading() {
        return mLoading;
    }

    @Override
    protected void onFinishInflate() {
        super.onFinishInflate();

        mProgressDrawable = (ClipDrawable) findViewById(R.id.toolbar).getBackground();
        initializeUrlField();
        initializeNavigationButtons();
    }

    private void initializeUrlField() {
        mUrlTextView = (EditText) findViewById(R.id.url);
        mUrlTextView.setOnEditorActionListener(new OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                if ((actionId != EditorInfo.IME_ACTION_GO) && (event == null ||
                        event.getKeyCode() != KeyEvent.KEYCODE_ENTER ||
                        event.getAction() != KeyEvent.ACTION_DOWN)) {
                    return false;
                }
                loadUrl(mUrlTextView.getText().toString());
                setKeyboardVisibilityForUrl(false);
                mContentViewCore.getContainerView().requestFocus();
                return true;
            }
        });
        mUrlTextView.setOnFocusChangeListener(new OnFocusChangeListener() {
            @Override
            public void onFocusChange(View v, boolean hasFocus) {
                setKeyboardVisibilityForUrl(hasFocus);
                mNextButton.setVisibility(hasFocus ? GONE : VISIBLE);
                mPrevButton.setVisibility(hasFocus ? GONE : VISIBLE);
                if (!hasFocus) {
                    mUrlTextView.setText(mWebContents.getUrl());
                }
            }
        });
        mUrlTextView.setOnKeyListener(new OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                if (keyCode == KeyEvent.KEYCODE_BACK) {
                    mContentViewCore.getContainerView().requestFocus();
                    return true;
                }
                return false;
            }
        });
    }

    /**
     * Loads an URL.  This will perform minimal amounts of sanitizing of the URL to attempt to
     * make it valid.
     *
     * @param url The URL to be loaded by the shell.
     */
    public void loadUrl(String url) {
        if (url == null) return;

        if (TextUtils.equals(url, mWebContents.getUrl())) {
            mNavigationController.reload(true);
        } else {
            mNavigationController.loadUrl(new LoadUrlParams(sanitizeUrl(url)));//加载参数url描述的URL,这里的mNavigationController是NavigationControllerImpl对象，而在老罗讲的5.0里面这个是ContentViewCore
        }
        mUrlTextView.clearFocus();
        // TODO(aurimas): Remove this when crbug.com/174541 is fixed.
        mContentViewCore.getContainerView().clearFocus();
        mContentViewCore.getContainerView().requestFocus();
    }

    /**
     * Given an URL, this performs minimal sanitizing to ensure it will be valid.
     * @param url The url to be sanitized.
     * @return The sanitized URL.
     */
    public static String sanitizeUrl(String url) {
        if (url == null) return null;
        if (url.startsWith("www.") || url.indexOf(":") == -1) url = "http://" + url;
        return url;
    }

    private void initializeNavigationButtons() {
        mPrevButton = (ImageButton) findViewById(R.id.prev);
        mPrevButton.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                if (mNavigationController.canGoBack()) mNavigationController.goBack();
            }
        });

        mNextButton = (ImageButton) findViewById(R.id.next);
        mNextButton.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                if (mNavigationController.canGoForward()) mNavigationController.goForward();
            }
        });
        mStopButton = (ImageButton) findViewById(R.id.stop);
        mStopButton.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                if (mLoading) mWebContents.stop();
            }
        });
        mReloadButton = (ImageButton) findViewById(R.id.reload);
        mReloadButton.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                mNavigationController.reload(true);
            }
        });
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onUpdateUrl(String url) {
        mUrlTextView.setText(url);
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void onLoadProgressChanged(double progress) {
        removeCallbacks(mClearProgressRunnable);
        mProgressDrawable.setLevel((int) (10000.0 * progress));
        if (progress == 1.0) postDelayed(mClearProgressRunnable, COMPLETED_PROGRESS_TIMEOUT_MS);
    }

    @CalledByNative
    private void toggleFullscreenModeForTab(boolean enterFullscreen) {
    }

    @CalledByNative
    private boolean isFullscreenForTabOrPending() {
        return false;
    }

    @SuppressWarnings("unused")
    @CalledByNative
    private void setIsLoading(boolean loading) {
        mLoading = loading;
    }

    /**
     * Initializes the ContentView based on the native tab contents pointer passed in.
     * @param nativeWebContents The pointer to the native tab contents object.
     */
    @SuppressWarnings("unused")
    @CalledByNative
    private void initFromNativeTabContents(long nativeWebContents) {
        Context context = getContext();
        mContentViewCore = new ContentViewCore(context);//创建ContentViewCore对象
        ContentView cv = ContentView.newInstance(context, mContentViewCore);
        mContentViewCore.initialize(cv, cv, nativeWebContents, mWindow);
        mContentViewCore.setContentViewClient(mContentViewClient);
        mWebContents = mContentViewCore.getWebContents();
        mNavigationController = mWebContents.getNavigationController();
        if (getParent() != null) mContentViewCore.onShow();
        if (mWebContents.getUrl() != null) {
            mUrlTextView.setText(mWebContents.getUrl());
        }
        ((FrameLayout) findViewById(R.id.contentview_holder)).addView(cv,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
        cv.requestFocus();
        mContentViewRenderView.setCurrentContentViewCore(mContentViewCore);
    }

    /**
     * Enable/Disable navigation(Prev/Next) button if navigation is allowed/disallowed
     * in respective direction.
     * @param controlId Id of button to update
     * @param enabled enable/disable value
     */
    @CalledByNative
    private void enableUiControl(int controlId, boolean enabled) {
        if (controlId == 0) mPrevButton.setEnabled(enabled);
        else if (controlId == 1) mNextButton.setEnabled(enabled);
        else if (controlId == 2) {
            mStopButton.setVisibility(enabled ? VISIBLE : GONE);
            mReloadButton.setVisibility(enabled ? GONE : VISIBLE);
        }
    }

    /**
     * @return The {@link ViewGroup} currently shown by this Shell.
     */
    public ViewGroup getContentView() {
        return mContentViewCore.getContainerView();
    }

    /**
     * @return The {@link ContentViewCore} currently managing the view shown by this Shell.
     */
    public ContentViewCore getContentViewCore() {
        return mContentViewCore;
    }

    private void setKeyboardVisibilityForUrl(boolean visible) {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(
                Context.INPUT_METHOD_SERVICE);
        if (visible) {
            imm.showSoftInput(mUrlTextView, InputMethodManager.SHOW_IMPLICIT);
        } else {
            imm.hideSoftInputFromWindow(mUrlTextView.getWindowToken(), 0);
        }
    }

    private static native void nativeCloseShell(long shellPtr);
}
