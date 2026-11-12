package com.remote360.client;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.nio.ByteBuffer;

public final class MainActivity extends Activity implements UdpClient.Listener {
    private static final String PREFS = "remote360";
    private static final String PREF_IP = "xbox_ip";
    private static final String PREF_RESOLUTION = "resolution";
    private static final String PREF_FRAME_RATE = "frame_rate";
    private static final String PREF_FAST_VIDEO = "fast_video";
    private static final String DEFAULT_IP = "";

    private final GamepadState gamepad = new GamepadState();
    private final Handler main = new Handler(Looper.getMainLooper());
    private EditText ipField;
    private TextView statusView;
    private TextView playbackNotice, playbackStats, resolutionNotice;
    private CheckBox videoOption;
    private CheckBox fastVideoOption;
    private Spinner resolutionOption, frameRateOption;
    private ImageView videoView;
    private LinearLayout connectPanel;
    private ScrollView connectOverlay;
    private boolean receivedVideo;
    private Bitmap videoBitmap;
    private Bitmap rawBitmap;
    private volatile UdpClient client;
    private Thread clientThread;
    private long lastStatusAt;
    private long lastFrameAt;
    private long fpsWindowStart;
    private int framesInWindow, displayedFps;
    private final Object frameLock = new Object();
    private UdpClient.VideoFrame pendingFrame;
    private UdpClient pendingSource;
    private boolean presentationScheduled;
    private UdpClient.VideoFrame drawingFrame;
    private UdpClient drawingSource;
    private Protocol.VideoStatus xboxVideoStatus;
    private boolean xboxSendFailed;
    private Vibrator activeVibrator;
    private final Runnable connectionHealth = new Runnable() {
        @Override public void run() {
            if (client == null) return;
            if (SystemClock.elapsedRealtime() - lastStatusAt > 1500) {
                statusView.setText("Xbox sem resposta. Confira o IP, a rede e o plugin.");
            }
            updatePlaybackNotice();
            main.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        buildUi();
        immersive();
    }

    private void buildUi() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        videoView = new ImageView(this) {
            @Override protected void onDraw(Canvas canvas) {
                super.onDraw(canvas);
                recordDrawnFrame();
            }
        };
        videoView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        videoView.setBackgroundColor(Color.BLACK);
        videoView.setVisibility(View.INVISIBLE);
        videoView.setOnClickListener(v -> showPanel());
        root.addView(videoView, new FrameLayout.LayoutParams(-1, -1));

        playbackStats = new TextView(this);
        playbackStats.setTextColor(Color.WHITE);
        playbackStats.setTextSize(12);
        playbackStats.setBackgroundColor(0x99000000);
        playbackStats.setPadding(dp(8), dp(4), dp(8), dp(4));
        playbackStats.setVisibility(View.GONE);
        FrameLayout.LayoutParams statsParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        statsParams.setMargins(dp(8), dp(8), dp(8), dp(8));
        root.addView(playbackStats, statsParams);

        playbackNotice = new TextView(this);
        playbackNotice.setTextColor(Color.WHITE);
        playbackNotice.setTextSize(14);
        playbackNotice.setBackgroundColor(0xD9000000);
        int padding = Math.round(12 * getResources().getDisplayMetrics().density);
        playbackNotice.setPadding(padding, padding / 2, padding, padding / 2);
        playbackNotice.setVisibility(View.GONE);
        FrameLayout.LayoutParams noticeParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        noticeParams.bottomMargin = padding;
        root.addView(playbackNotice, noticeParams);

        connectPanel = new LinearLayout(this);
        connectPanel.setOrientation(LinearLayout.VERTICAL);
        connectPanel.setGravity(Gravity.CENTER);
        connectPanel.setPadding(dp(16), dp(12), dp(16), dp(12));
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(-1, -1);
        connectOverlay = new ScrollView(this);
        connectOverlay.setFillViewport(true);
        connectOverlay.setBackgroundColor(0xDD000000);
        int panelWidth = Math.min(getResources().getDisplayMetrics().widthPixels, dp(540));
        connectOverlay.addView(connectPanel, new ScrollView.LayoutParams(panelWidth, -2, Gravity.CENTER_HORIZONTAL));
        root.addView(connectOverlay, panelParams);

        TextView title = new TextView(this);
        title.setText("Remote360");
        title.setTextColor(Color.WHITE);
        title.setTextSize(30);
        title.setGravity(Gravity.CENTER);
        connectPanel.addView(title, new LinearLayout.LayoutParams(-1, -2));

        TextView subtitle = new TextView(this);
        subtitle.setText("Xbox 360 RGH");
        subtitle.setTextColor(0xFFB0B0B0);
        subtitle.setTextSize(15);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = new LinearLayout.LayoutParams(-1, -2);
        subtitleParams.setMargins(0, dp(2), 0, dp(8));
        connectPanel.addView(subtitle, subtitleParams);

        ipField = new EditText(this);
        ipField.setSingleLine(true);
        ipField.setText(prefs.getString(PREF_IP, DEFAULT_IP));
        ipField.setHint("IP do Xbox");
        ipField.setTextColor(Color.WHITE);
        ipField.setHintTextColor(0xFF707070);
        ipField.setGravity(Gravity.CENTER);
        connectPanel.addView(ipField, new LinearLayout.LayoutParams(dp(220), -2));

        videoOption = new CheckBox(this);
        videoOption.setText("Ativar vídeo experimental (sem áudio)");
        videoOption.setTextColor(Color.WHITE);
        videoOption.setOnCheckedChangeListener((button, checked) -> {
            if (client == null) return;
            client.setVideoEnabled(checked);
            xboxVideoStatus = null;
            xboxSendFailed = false;
            if (checked) {
                statusView.setText("Solicitando vídeo ao Xbox…");
            } else {
                receivedVideo = false;
                lastFrameAt = 0;
                videoView.setVisibility(View.INVISIBLE);
                showPanel();
            }
        });
        connectPanel.addView(videoOption, new LinearLayout.LayoutParams(-2, -2));
        fastVideoOption = new CheckBox(this);
        fastVideoOption.setText("Priorizar fluidez");
        fastVideoOption.setTextColor(Color.WHITE);
        fastVideoOption.setChecked(prefs.getBoolean(PREF_FAST_VIDEO, true));
        connectPanel.addView(fastVideoOption, new LinearLayout.LayoutParams(-2, -2));
        LinearLayout videoSettings = new LinearLayout(this);
        videoSettings.setGravity(Gravity.CENTER);
        connectPanel.addView(videoSettings, new LinearLayout.LayoutParams(-1, -2));

        String[] resolutions = new String[Protocol.WIDTHS.length];
        for (int i = 0; i < resolutions.length; i++) {
            resolutions[i] = Protocol.WIDTHS[i] + " × " + Protocol.HEIGHTS[i];
        }
        String[] rates = new String[Protocol.FRAME_RATES.length];
        for (int i = 0; i < rates.length; i++) {
            rates[i] = Protocol.FRAME_RATES[i] + " FPS";
        }
        resolutionOption = addSetting(videoSettings, "Resolução", resolutions, prefs.getInt(PREF_RESOLUTION, 0));
        frameRateOption = addSetting(videoSettings, "FPS desejado", rates, prefs.getInt(PREF_FRAME_RATE, 2));
        resolutionNotice = new TextView(this);
        resolutionNotice.setText("Essa resolução usa o modo qualidade. A imagem e os FPS dependem do Xbox.");
        resolutionNotice.setTextColor(0xFFB0B0B0);
        resolutionNotice.setGravity(Gravity.CENTER);
        resolutionNotice.setVisibility(View.GONE);
        connectPanel.addView(resolutionNotice, new LinearLayout.LayoutParams(-1, -2));
        AdapterView.OnItemSelectedListener updateSettings = new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                applyVideoSettings();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        };
        resolutionOption.setOnItemSelectedListener(updateSettings);
        frameRateOption.setOnItemSelectedListener(updateSettings);
        fastVideoOption.setOnCheckedChangeListener((button, checked) -> applyVideoSettings());

        Button connect = new Button(this);
        connect.setText("Conectar");
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER);
        connectPanel.addView(actions, new LinearLayout.LayoutParams(-1, -2));
        actions.addView(connect, new LinearLayout.LayoutParams(0, -2, 1));

        Button disconnect = new Button(this);
        disconnect.setText("Desconectar");
        disconnect.setOnClickListener(v -> {
            stopClient();
            statusView.setText("Desconectado");
        });

        Button viewVideo = new Button(this);
        viewVideo.setText("Ver imagem");
        viewVideo.setOnClickListener(v -> {
            videoOption.setChecked(true);
            if (client == null) {
                connect();
            } else if (receivedVideo) {
                showVideo();
            } else {
                statusView.setText("Vídeo solicitado. Aguardando um quadro completo do Xbox…");
            }
        });
        actions.addView(viewVideo, new LinearLayout.LayoutParams(0, -2, 1));
        actions.addView(disconnect, new LinearLayout.LayoutParams(0, -2, 1));

        statusView = new TextView(this);
        statusView.setText("Aguardando conexão");
        statusView.setTextColor(0xFFB0B0B0);
        statusView.setTextSize(16);
        statusView.setGravity(Gravity.CENTER);
        connectPanel.addView(statusView, new LinearLayout.LayoutParams(-1, -2));

        TextView help = new TextView(this);
        help.setText("Toque na imagem para abrir este painel. Voltar ou sair do app desconecta.");
        help.setTextColor(0xFF707070);
        help.setTextSize(13);
        help.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams helpParams = new LinearLayout.LayoutParams(-1, -2);
        helpParams.setMargins(0, dp(8), 0, 0);
        connectPanel.addView(help, helpParams);

        connect.setOnClickListener(v -> connect());
        setContentView(root);
        applyVideoSettings();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private Spinner addSetting(LinearLayout row, String title, String[] choices, int selection) {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams columnParams = new LinearLayout.LayoutParams(0, -2, 1);
        columnParams.setMargins(8, 8, 8, 8);
        row.addView(column, columnParams);

        TextView label = new TextView(this);
        label.setText(title);
        label.setTextColor(0xffb0b0b0);
        column.addView(label);

        Spinner option = new Spinner(this, Spinner.MODE_DROPDOWN);
        option.setId(View.generateViewId());
        option.setContentDescription(title);
        label.setLabelFor(option.getId());
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, choices);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        option.setAdapter(adapter);
        option.setSelection(Math.max(0, Math.min(selection, choices.length - 1)));
        column.addView(option, new LinearLayout.LayoutParams(-1, -2));
        return option;
    }

    private void applyVideoSettings() {
        int resolution = resolutionOption.getSelectedItemPosition();
        int rate = frameRateOption.getSelectedItemPosition();
        if (resolution < 0 || rate < 0) return;
        boolean fastAvailable = Protocol.supportsFastVideo(resolution);
        fastVideoOption.setVisibility(fastAvailable ? View.VISIBLE : View.GONE);
        resolutionNotice.setVisibility(fastAvailable ? View.GONE : View.VISIBLE);
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putInt(PREF_RESOLUTION, resolution).putInt(PREF_FRAME_RATE, rate)
                .putBoolean(PREF_FAST_VIDEO, fastVideoOption.isChecked()).apply();
        if (client != null) {
            client.setVideoSettings(resolution, rate, isFastVideoRequested());
            resetFps();
        }
    }

    private boolean isFastVideoRequested() {
        return fastVideoOption.getVisibility() == View.VISIBLE && fastVideoOption.isChecked();
    }

    private void connect() {
        String host = ipField.getText().toString().trim();
        if (host.isEmpty()) return;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_IP, host).apply();
        stopClient();
        client = new UdpClient(host, gamepad, videoOption.isChecked(), this);
        applyVideoSettings();
        clientThread = new Thread(client, "Remote360-UDP");
        clientThread.start();
        lastStatusAt = SystemClock.elapsedRealtime();
        main.postDelayed(connectionHealth, 500);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        statusView.setText("Enviando para " + host + ":" + Protocol.PORT + "…");
        hideKeyboard();
        ipField.clearFocus();
        getWindow().getDecorView().requestFocus();
    }

    private void stopClient() {
        gamepad.clear();
        if (client != null) client.stop();
        client = null;
        clientThread = null;
        receivedVideo = false;
        lastFrameAt = 0;
        resetFps();
        synchronized (frameLock) {
            pendingFrame = null;
            pendingSource = null;
        }
        drawingSource = null;
        drawingFrame = null;
        xboxVideoStatus = null;
        xboxSendFailed = false;
        main.removeCallbacks(connectionHealth);
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (activeVibrator != null) activeVibrator.cancel();
        activeVibrator = null;
        showPanel();
        if (videoView != null) {
            videoView.setVisibility(View.INVISIBLE);
            videoView.setImageDrawable(null);
        }
        rawBitmap = null;
        videoBitmap = null;
    }

    private void resetFps() {
        fpsWindowStart = 0;
        framesInWindow = 0;
        displayedFps = 0;
    }

    private void showPanel() {
        if (connectOverlay != null) connectOverlay.setVisibility(View.VISIBLE);
        if (playbackNotice != null) playbackNotice.setVisibility(View.GONE);
        if (playbackStats != null) playbackStats.setVisibility(View.GONE);
    }

    private void showVideo() {
        connectOverlay.setVisibility(View.INVISIBLE);
        updatePlaybackNotice();
    }

    private boolean videoIsCurrent() {
        return receivedVideo && SystemClock.elapsedRealtime() - lastFrameAt < 2000;
    }

    private void updatePlaybackNotice() {
        boolean playing = receivedVideo && connectOverlay.getVisibility() != View.VISIBLE;
        boolean current = videoIsCurrent();
        playbackStats.setVisibility(playing && current ? View.VISIBLE : View.GONE);
        if (playing && current) {
            String stats = videoBitmap.getWidth() + " × " + videoBitmap.getHeight()
                    + (displayedFps > 0 ? " · " + displayedFps + " FPS" : "");
            if (!stats.contentEquals(playbackStats.getText())) playbackStats.setText(stats);
        }
        if (!playing || current) {
            playbackNotice.setVisibility(View.GONE);
            return;
        }
        playbackNotice.setText(SystemClock.elapsedRealtime() - lastStatusAt > 1500
                ? "Imagem congelada · Xbox sem resposta"
                : "Imagem congelada · " + videoWaitMessage());
        playbackNotice.setVisibility(View.VISIBLE);
    }

    private String videoWaitMessage() {
        if (xboxVideoStatus != null && xboxVideoStatus.requested) {
            switch (xboxVideoStatus.captureResult) {
                case 1:
                case 3:
                    return "Xbox não conseguiu ler a imagem";
                case 2:
                    return "formato de imagem ainda não suportado";
                case 4:
                case 5:
                    return "aguardando uma imagem estável no Xbox";
                case 7:
                    return "Xbox ajustando a compressão";
                default:
                    if (xboxSendFailed) return "Xbox encontrou erros ao enviar os quadros";
            }
        }
        return receivedVideo ? "aguardando o próximo quadro" : "aguardando o primeiro quadro completo";
    }

    @Override
    protected void onPause() {
        stopClient();
        statusView.setText("Pausado. Toque em Conectar para retomar.");
        super.onPause();
    }

    @Override
    public void onBackPressed() {
        if (client != null) {
            stopClient();
            statusView.setText("Desconectado");
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        stopClient();
        super.onDestroy();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (client != null && gamepad.onKey(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (client != null && event.getAction() == MotionEvent.ACTION_MOVE && gamepad.onMotion(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onStatus(UdpClient source, Protocol.Status status, long ackMs) {
        main.post(() -> {
            if (client != source) return;
            lastStatusAt = SystemClock.elapsedRealtime();
            String mode;
            if (status.controllerConflict) mode = "outro driver de controle está ativo";
            else if (status.controllerFailed) mode = "Xbox não registrou o controle";
            else if (status.videoOnly) mode = "modo vídeo";
            else mode = status.bound ? "controle OK" : "mexa no controle do Thor para ativar";
            String video = "";
            if (videoOption.isChecked()) {
                if (videoIsCurrent()) {
                    video = "\nExibindo " + videoBitmap.getWidth() + " × " + videoBitmap.getHeight()
                            + (displayedFps > 0 ? " · " + displayedFps + " FPS" : "");
                } else {
                    video = "\n" + (receivedVideo ? "Vídeo interrompido" : "Vídeo solicitado")
                            + " · " + videoWaitMessage();
                }
                if (xboxVideoStatus != null && !xboxVideoStatus.configurable) {
                    video += "\nAtualize o módulo do Xbox para aplicar a resolução e os FPS escolhidos.";
                } else if (xboxVideoStatus != null && isFastVideoRequested() && !xboxVideoStatus.fastSupported) {
                    video += "\nCarregue o módulo novo no Xbox para ativar a fluidez.";
                } else if (xboxVideoStatus != null && isFastVideoRequested() && xboxVideoStatus.captureLimited) {
                    video += "\nModo fluidez ajustou a resolução.";
                } else if (xboxVideoStatus != null && xboxVideoStatus.sourceLimited) {
                    video += "\nO jogo fornece " + xboxVideoStatus.sourceWidth + " × " + xboxVideoStatus.sourceHeight + ".";
                } else if (xboxVideoStatus != null && xboxVideoStatus.encoderLimited) {
                    video += "\nXbox reduziu a qualidade para manter o vídeo.";
                } else if (xboxVideoStatus != null && xboxVideoStatus.captureLimited) {
                    video += "\nXbox reduziu a resolução para estabilizar a captura.";
                }
            } else if (status.videoOnly) {
                video = "\nToque em Ver imagem para solicitar vídeo.";
            }
            statusView.setText(
                "Xbox respondeu · " + mode
                        + " · ACK " + status.ack
                        + (ackMs >= 0 ? " · resposta " + ackMs + " ms" : "")
                        + (status.watchdog ? " · sem comandos" : "")
                        + video);
        });
    }

    @Override
    public void onVideoStatus(UdpClient source, Protocol.VideoStatus status) {
        main.post(() -> {
            if (client != source || !videoOption.isChecked()) return;
            if (xboxVideoStatus != null) {
                if (status.framesSent != xboxVideoStatus.framesSent) xboxSendFailed = false;
                if (status.sendErrors != xboxVideoStatus.sendErrors) xboxSendFailed = true;
            }
            xboxVideoStatus = status;
        });
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onRumble(UdpClient source, Protocol.Rumble rumble) {
        int strength = Math.max(rumble.left, rumble.right);
        main.post(() -> {
            if (client != source) return;
            if (strength <= 0) {
                if (activeVibrator != null) activeVibrator.cancel();
                return;
            }
            InputDevice device = InputDevice.getDevice(gamepad.deviceId());
            Vibrator vibrator = device == null ? null : device.getVibrator();
            if (vibrator == null || !vibrator.hasVibrator()) {
                vibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            }
            if (vibrator != null && vibrator.hasVibrator()) {
                int amplitude = Math.max(1, Math.min(255, strength * 255 / 100));
                activeVibrator = vibrator;
                vibrator.vibrate(VibrationEffect.createOneShot(110, amplitude));
            }
        });
    }

    @Override
    public void onVideoFrame(UdpClient source, UdpClient.VideoFrame frame) {
        synchronized (frameLock) {
            if (client != source) return;
            pendingFrame = frame;
            pendingSource = source;
            if (!presentationScheduled) {
                presentationScheduled = true;
                main.post(() -> videoView.postOnAnimation(this::presentVideoFrame));
            }
        }
    }

    private void presentVideoFrame() {
        UdpClient source;
        UdpClient.VideoFrame frame;
        synchronized (frameLock) {
            source = pendingSource;
            frame = pendingFrame;
            pendingSource = null;
            pendingFrame = null;
            presentationScheduled = false;
        }
        if (frame == null || client != source || !source.isCurrentVideo(frame)) return;

        Bitmap next = frame.bitmap;
        if (next == null) {
            if (rawBitmap == null || rawBitmap.getWidth() != frame.width || rawBitmap.getHeight() != frame.height) {
                rawBitmap = Bitmap.createBitmap(frame.width, frame.height, Bitmap.Config.RGB_565);
            }
            rawBitmap.copyPixelsFromBuffer(ByteBuffer.wrap(frame.rgb565));
            next = rawBitmap;
        }
        drawingSource = source;
        drawingFrame = frame;
        if (next != videoBitmap) {
            videoBitmap = next;
            videoView.setImageBitmap(next);
        }
        videoView.setVisibility(View.VISIBLE);
        if (!receivedVideo) {
            receivedVideo = true;
            connectOverlay.setVisibility(View.INVISIBLE);
        }
        videoView.invalidate();
    }

    private void recordDrawnFrame() {
        UdpClient source = drawingSource;
        UdpClient.VideoFrame frame = drawingFrame;
        drawingSource = null;
        drawingFrame = null;
        if (frame == null || client != source || !source.isCurrentVideo(frame)) return;
        long now = SystemClock.elapsedRealtime();
        if (lastFrameAt == 0 || now - lastFrameAt >= 2000) resetFps();
        if (fpsWindowStart == 0) {
            fpsWindowStart = now;
        } else {
            framesInWindow++;
        }
        if (now - fpsWindowStart >= 1000) {
            displayedFps = (int) Math.round(1000.0 * framesInWindow / (now - fpsWindowStart));
            framesInWindow = 0;
            fpsWindowStart = now;
        }
        lastFrameAt = now;
        source.frameRendered(frame.id);
    }

    @Override
    public void onNetworkError(UdpClient source, String message) {
        main.post(() -> {
            if (client != source) return;
            stopClient();
            statusView.setText("Erro de rede · " + message);
        });
    }

    private void hideKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(ipField.getWindowToken(), 0);
    }

    private void immersive() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        if (android.os.Build.VERSION.SDK_INT >= 30 && getWindow().getInsetsController() != null) {
            getWindow().getInsetsController().hide(WindowInsets.Type.statusBars()
                    | WindowInsets.Type.navigationBars());
        }
    }
}

