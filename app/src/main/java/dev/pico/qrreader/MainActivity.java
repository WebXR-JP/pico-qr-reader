package dev.pico.qrreader;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.Log;
import android.widget.Toast;

import java.nio.ByteBuffer;

public class MainActivity extends Activity {
    private static final String TAG = "PicoQrReader";
    private static final int REQ_CAMERA = 1001;

    static {
        System.loadLibrary("openxr_loader");
        System.loadLibrary("pico_qr_reader");
    }

    private Thread xrThread;
    private volatile boolean running = false;

    private native void nativeMainLoop(Activity activity);
    private native void nativeRequestStop();
    private native void nativeOnCameraPermissionResult(boolean granted);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "MainActivity.onCreate");

        if (!hasCameraPermission()) {
            requestCameraPermission();
        }

        running = true;
        final Activity self = this;
        xrThread = new Thread(new Runnable() {
            @Override
            public void run() {
                Log.i(TAG, "XR thread started");
                nativeMainLoop(self);
                Log.i(TAG, "XR thread exited");
            }
        }, "PicoQrXrThread");
        xrThread.start();
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "MainActivity.onDestroy");
        running = false;
        nativeRequestStop();
        if (xrThread != null) {
            try {
                xrThread.join(3000);
            } catch (InterruptedException ignored) {
            }
            xrThread = null;
        }
        super.onDestroy();
    }

    public boolean hasCameraPermission() {
        return checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
    }

    public void requestCameraPermission() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
                }
            }
        });
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_CAMERA) {
            boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            Log.i(TAG, "CAMERA permission result: " + granted);
            nativeOnCameraPermissionResult(granted);
        }
    }

    public void copyToClipboard(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cm != null) {
                        cm.setPrimaryClip(ClipData.newPlainText("QR Code", text));
                        Toast.makeText(MainActivity.this, "Copied QR to clipboard", Toast.LENGTH_SHORT).show();
                        Log.i(TAG, "Copied to clipboard: " + text);
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Failed to copy to clipboard", e);
                }
            }
        });
    }

    public void openUrl(final String url) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(intent);
                    Log.i(TAG, "Opened URL: " + url);
                } catch (Exception e) {
                    Log.e(TAG, "Failed to open URL: " + url, e);
                }
            }
        });
    }

    public byte[] renderHudBitmap(int width, int height, String status, String qrText, boolean isUrl, boolean detected) {
        Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);

        // Background rounded panel
        Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        bg.setColor(Color.argb(220, 18, 22, 30));
        RectF rect = new RectF(4, 4, width - 4, height - 4);
        canvas.drawRoundRect(rect, 24, 24, bg);

        // Border
        Paint border = new Paint(Paint.ANTI_ALIAS_FLAG);
        border.setStyle(Paint.Style.STROKE);
        border.setStrokeWidth(6);
        border.setColor(detected ? Color.rgb(46, 224, 128) : Color.rgb(80, 160, 255));
        canvas.drawRoundRect(rect, 24, 24, border);

        // Title + status
        Paint titlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        titlePaint.setColor(detected ? Color.rgb(46, 224, 128) : Color.rgb(130, 200, 255));
        titlePaint.setTextSize(34);
        titlePaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        canvas.drawText("PICO QR Reader  |  " + (status != null ? status : ""), 32, 54, titlePaint);

        // Divider
        Paint div = new Paint();
        div.setColor(Color.argb(90, 255, 255, 255));
        div.setStrokeWidth(2);
        canvas.drawLine(28, 72, width - 28, 72, div);

        // QR content
        TextPaint bodyPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        bodyPaint.setColor(Color.WHITE);
        bodyPaint.setTextSize(32);
        String body = (qrText != null && !qrText.isEmpty())
                ? qrText
                : "QRコードをカメラに向けてください...";
        StaticLayout layout = StaticLayout.Builder
                .obtain(body, 0, body.length(), bodyPaint, width - 64)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL)
                .setMaxLines(4)
                .build();
        canvas.save();
        canvas.translate(32, 90);
        layout.draw(canvas);
        canvas.restore();

        // Footer hint
        Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        hintPaint.setColor(Color.rgb(180, 195, 215));
        hintPaint.setTextSize(24);
        String hint;
        if (qrText != null && !qrText.isEmpty()) {
            hint = isUrl
                    ? "[クリップボードにコピー済み]  右トリガー: ブラウザで開く"
                    : "[クリップボードにコピー済み]  右トリガー: 再コピー";
        } else {
            hint = "外向きRGBカメラ (XR_PICO_camera_image) で自動スキャン中";
        }
        canvas.drawText(hint, 32, height - 24, hintPaint);

        ByteBuffer buf = ByteBuffer.allocate(width * height * 4);
        bmp.copyPixelsToBuffer(buf);
        bmp.recycle();
        return buf.array();
    }
}
