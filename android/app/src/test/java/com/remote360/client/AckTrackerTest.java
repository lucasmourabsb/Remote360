package com.remote360.client;

import org.junit.Test;
import static org.junit.Assert.*;

public class AckTrackerTest {
    @Test public void samplesTheAcknowledgedPacketInsteadOfTheLatestSend() {
        AckTracker tracker = new AckTracker();
        tracker.sent(1, 10_000_000L);
        tracker.sent(2, 18_000_000L);
        assertEquals(25, tracker.acknowledge(1, 35_000_000L));
        assertEquals(17, tracker.acknowledge(2, 35_000_000L));
    }

    @Test public void unknownDuplicateAndOverwrittenPacketsHaveNoSample() {
        AckTracker tracker = new AckTracker();
        assertEquals(-1, tracker.acknowledge(0, 0));
        tracker.sent(1, 0);
        tracker.sent(1025, 1_000_000L);
        assertEquals(-1, tracker.acknowledge(1, 2_000_000L));
        assertEquals(1, tracker.acknowledge(1025, 2_000_000L));
        assertEquals(-1, tracker.acknowledge(1025, 3_000_000L));
    }

    @Test public void unsignedSequenceWrapKeepsTheCorrectSample() {
        AckTracker tracker = new AckTracker();
        tracker.sent(-1, 0);
        tracker.sent(0, 1_000_000L);
        assertEquals(2, tracker.acknowledge(0xFFFFFFFFL, 2_000_000L));
        assertEquals(1, tracker.acknowledge(0, 2_000_000L));
    }
}
