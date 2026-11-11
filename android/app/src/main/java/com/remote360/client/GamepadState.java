package com.remote360.client;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

final class GamepadState {
    private int buttons;
    private int hatButtons;
    private boolean digitalLeftTrigger, digitalRightTrigger;
    private boolean guide;
    private int leftTrigger;
    private int rightTrigger;
    private int leftX;
    private int leftY;
    private int rightX;
    private int rightY;
    private int deviceId = -1;

    synchronized boolean onKey(KeyEvent event) {
        int keyCode = event.getKeyCode();
        if (!event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                && !event.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
            boolean direction = keyCode == KeyEvent.KEYCODE_DPAD_UP || keyCode == KeyEvent.KEYCODE_DPAD_DOWN
                    || keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT;
            InputDevice device = event.getDevice();
            boolean controllerDevice = device != null && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                    || device.supportsSource(InputDevice.SOURCE_JOYSTICK));
            if (!(direction && event.isFromSource(InputDevice.SOURCE_DPAD)) && !controllerDevice) return false;
        }
        if (event.getAction() != KeyEvent.ACTION_DOWN && event.getAction() != KeyEvent.ACTION_UP) {
            return false;
        }
        deviceId = event.getDeviceId();
        int mask;
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: mask = Protocol.BTN_A; break;
            case KeyEvent.KEYCODE_BUTTON_B: mask = Protocol.BTN_B; break;
            case KeyEvent.KEYCODE_BUTTON_X: mask = Protocol.BTN_X; break;
            case KeyEvent.KEYCODE_BUTTON_Y: mask = Protocol.BTN_Y; break;
            case KeyEvent.KEYCODE_BUTTON_L1: mask = Protocol.BTN_SHOULDER_L; break;
            case KeyEvent.KEYCODE_BUTTON_R1: mask = Protocol.BTN_SHOULDER_R; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: mask = Protocol.BTN_THUMB_L; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: mask = Protocol.BTN_THUMB_R; break;
            case KeyEvent.KEYCODE_BUTTON_START: mask = Protocol.BTN_START; break;
            case KeyEvent.KEYCODE_BUTTON_SELECT: mask = Protocol.BTN_BACK; break;
            case KeyEvent.KEYCODE_DPAD_UP: mask = Protocol.BTN_DPAD_UP; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: mask = Protocol.BTN_DPAD_DOWN; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: mask = Protocol.BTN_DPAD_LEFT; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: mask = Protocol.BTN_DPAD_RIGHT; break;
            case KeyEvent.KEYCODE_BUTTON_MODE:
                guide = event.getAction() != KeyEvent.ACTION_UP;
                return true;
            case KeyEvent.KEYCODE_BUTTON_L2:
                digitalLeftTrigger = !hasTriggerAxes(event, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE)
                        && event.getAction() != KeyEvent.ACTION_UP;
                return true;
            case KeyEvent.KEYCODE_BUTTON_R2:
                digitalRightTrigger = !hasTriggerAxes(event, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS)
                        && event.getAction() != KeyEvent.ACTION_UP;
                return true;
            default:
                return false;
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN) buttons |= mask;
        if (event.getAction() == KeyEvent.ACTION_UP) buttons &= ~mask;
        return true;
    }

    synchronized boolean onMotion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) {
            return false;
        }
        deviceId = event.getDeviceId();
        leftX = signedAxis(event, MotionEvent.AXIS_X, false);
        leftY = signedAxis(event, MotionEvent.AXIS_Y, true);

        int rightXAxis = hasAxis(event, MotionEvent.AXIS_Z) ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX;
        int rightYAxis = hasAxis(event, MotionEvent.AXIS_RZ) ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY;
        rightX = signedAxis(event, rightXAxis, false);
        rightY = signedAxis(event, rightYAxis, true);

        int ltAxis = hasAxis(event, MotionEvent.AXIS_LTRIGGER)
                ? MotionEvent.AXIS_LTRIGGER : MotionEvent.AXIS_BRAKE;
        int rtAxis = hasAxis(event, MotionEvent.AXIS_RTRIGGER)
                ? MotionEvent.AXIS_RTRIGGER : MotionEvent.AXIS_GAS;
        leftTrigger = triggerAxis(event, ltAxis);
        rightTrigger = triggerAxis(event, rtAxis);

        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hatButtons = setBit(hatButtons, Protocol.BTN_DPAD_LEFT, hatX < -0.5f);
        hatButtons = setBit(hatButtons, Protocol.BTN_DPAD_RIGHT, hatX > 0.5f);
        hatButtons = setBit(hatButtons, Protocol.BTN_DPAD_UP, hatY < -0.5f);
        hatButtons = setBit(hatButtons, Protocol.BTN_DPAD_DOWN, hatY > 0.5f);
        return true;
    }

    synchronized void clear() {
        buttons = hatButtons = leftTrigger = rightTrigger = leftX = leftY = rightX = rightY = 0;
        guide = digitalLeftTrigger = digitalRightTrigger = false;
        deviceId = -1;
    }

    synchronized int deviceId() { return deviceId; }

    synchronized Snapshot snapshot() {
        return new Snapshot(buttons | hatButtons, guide,
                digitalLeftTrigger ? 255 : leftTrigger, digitalRightTrigger ? 255 : rightTrigger,
                leftX, leftY, rightX, rightY);
    }

    private static int setBit(int value, int mask, boolean enabled) {
        return enabled ? value | mask : value & ~mask;
    }

    private static boolean hasAxis(MotionEvent event, int axis) {
        InputDevice device = event.getDevice();
        return device != null && device.getMotionRange(axis, event.getSource()) != null;
    }

    private static boolean hasTriggerAxes(KeyEvent event, int first, int second) {
        InputDevice device = event.getDevice();
        return device != null && (device.getMotionRange(first) != null || device.getMotionRange(second) != null);
    }

    private static int signedAxis(MotionEvent event, int axis, boolean invert) {
        float value = event.getAxisValue(axis);
        if (Math.abs(value) < 0.04f) value = 0;
        if (invert) value = -value;
        value = Math.max(-1f, Math.min(1f, value));
        return Math.round(value * 32767f);
    }

    private static int triggerAxis(MotionEvent event, int axis) {
        float value = event.getAxisValue(axis);
        value = Math.max(0f, Math.min(1f, value));
        return Math.round(value * 255f);
    }

    static final class Snapshot {
        final int buttons;
        final boolean guide;
        final int leftTrigger;
        final int rightTrigger;
        final int leftX;
        final int leftY;
        final int rightX;
        final int rightY;

        Snapshot(int buttons, boolean guide, int leftTrigger, int rightTrigger, int leftX,
                 int leftY, int rightX, int rightY) {
            this.buttons = buttons;
            this.guide = guide;
            this.leftTrigger = leftTrigger;
            this.rightTrigger = rightTrigger;
            this.leftX = leftX;
            this.leftY = leftY;
            this.rightX = rightX;
            this.rightY = rightY;
        }
    }
}

