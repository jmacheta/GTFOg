package io.github.jmacheta.gtfog

import android.app.Application

class GtfogApp : Application() {
    val session by lazy { Session(this) }
}
