package io.github.jmacheta.gtfog

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class DfuTest {
    private fun image(major: Int, minor: Int, rev: Int, build: Long = 0): ByteArray {
        val b = ByteArray(64)
        byteArrayOf(0x3d, 0xb8.toByte(), 0xf3.toByte(), 0x96.toByte()).copyInto(b)
        b[20] = major.toByte()
        b[21] = minor.toByte()
        b[22] = rev.toByte()
        b[23] = (rev shr 8).toByte()
        for (i in 0..3) b[24 + i] = (build shr (8 * i)).toByte()
        return b
    }

    private fun zip(vararg e: Pair<String, ByteArray>): ByteArray {
        val o = ByteArrayOutputStream()
        ZipOutputStream(o).use { z -> e.forEach { (n, d) -> z.putNextEntry(ZipEntry(n)); z.write(d); z.closeEntry() } }
        return o.toByteArray()
    }

    @Test
    fun version() {
        assertEquals("0.2.0", parseImageVersion(image(0, 2, 0)))
        assertEquals("1.10.258+7", parseImageVersion(image(1, 10, 258, 7)))
    }

    @Test
    fun badHeader() {
        assertNull(parseImageVersion(ByteArray(64)))
        assertNull(parseImageVersion(ByteArray(10)))
        assertThrows(IllegalArgumentException::class.java) { loadImage(ByteArray(64)) }
    }

    @Test
    fun rawBin() {
        val i = image(0, 2, 0)
        assertArrayEquals(i, loadImage(i))
    }

    @Test
    fun zipPackage() {
        val i = image(0, 3, 1)
        assertArrayEquals(i, loadImage(zip("manifest.json" to "{}".toByteArray(), "zephyr.signed.bin" to i)))
        assertThrows(IllegalArgumentException::class.java) { loadImage(zip("manifest.json" to "{}".toByteArray())) }
        assertThrows(IllegalArgumentException::class.java) { loadImage(zip("a.bin" to i, "b.bin" to i)) }
        assertThrows(IllegalArgumentException::class.java) { loadImage(zip("a.bin" to ByteArray(64))) }
    }
}
