package tv.projector.receiver;

import android.os.Build;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * LAN discovery beacon: broadcasts "I am a Projector receiver" on UDP 45691 once a second so the
 * PC sender can list devices without knowing the TV's IP or scanning the subnet.
 *
 * <p>Payload is line-oriented (UTF-8), tolerant to extra keys:
 * <pre>
 * PJS/1
 * name=QINL-370A
 * ip=10.5.9.22
 * port=45690
 * mod=QINL-370A
 * </pre>
 * Broadcast goes to every interface's directed broadcast address (255.255.255.255 is often
 * filtered by AP isolation / routers, directed broadcast is more reliable).
 */
final class Discovery implements Runnable {
    static final int PORT = 45691;
    private static final String TAG = "pjs-disc";

    private final String name;
    private volatile boolean running = true;
    private Thread thread;

    Discovery(String name) {
        this.name = name != null && !name.isEmpty() ? name : Build.MODEL;
    }

    void start() {
        if (thread != null) return;
        thread = new Thread(this, "pjs-disc");
        thread.setDaemon(true);
        thread.start();
    }

    void stop() {
        running = false;
        if (thread != null) thread.interrupt();
    }

    @Override public void run() {
        while (running) {
            try {
                for (InetAddress b : broadcastAddrs()) {
                    DatagramSocket s = null;
                    try {
                        s = new DatagramSocket();
                        s.setBroadcast(true);
                        byte[] msg = payload(localIp(b));
                        s.send(new DatagramPacket(msg, msg.length, b, PORT));
                    } catch (Throwable ignored) {
                    } finally {
                        if (s != null) { try { s.close(); } catch (Throwable ignored) { } }
                    }
                }
            } catch (Throwable e) {
                Log.w(TAG, "beacon failed", e);
            }
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    private byte[] payload(String ip) {
        StringBuilder sb = new StringBuilder(96);
        sb.append("PJS/1\n");
        sb.append("name=").append(name).append('\n');
        sb.append("ip=").append(ip).append('\n');
        sb.append("port=").append(RxEngine.PORT).append('\n');
        sb.append("mod=").append(Build.MODEL).append('\n');
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    /** All directed broadcast addresses of up-links (IPv4). */
    private static List<InetAddress> broadcastAddrs() {
        List<InetAddress> out = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null && b.getAddress().length == 4) out.add(b);
                }
            }
        } catch (Throwable ignored) {
        }
        if (out.isEmpty()) {
            try { out.add(InetAddress.getByName("255.255.255.255")); } catch (Throwable ignored) { }
        }
        return out;
    }

    /** Local IPv4 address of the interface owning the given broadcast address. */
    private static String localIp(InetAddress bcast) {
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null && b.equals(bcast)) return ia.getAddress().getHostAddress();
                }
            }
        } catch (Throwable ignored) {
        }
        return "0.0.0.0";
    }
}
