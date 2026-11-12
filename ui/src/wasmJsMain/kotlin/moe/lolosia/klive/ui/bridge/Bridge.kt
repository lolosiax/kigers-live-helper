@file:OptIn(kotlin.js.ExperimentalWasmJsInterop::class)

package moe.lolosia.klive.ui.bridge

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.*

/** 桥不可用（浏览器开发模式）时抛出。 */
class BridgeUnavailableException : IllegalStateException("当前环境没有 WebView2 桥")

/** 原生层回包 `ok=false` 时抛出，message 直接来自 `klive_last_message()`。 */
class BridgeException(val code: Int, message: String) : IllegalStateException(message)

@JsFun("() => !!(globalThis.klive && globalThis.klive.available())")
private external fun bridgeAvailable(): Boolean

@JsFun("(message) => globalThis.klive.post(message)")
private external fun bridgePost(message: String)

@JsFun("(callback) => globalThis.klive.listen(callback)")
private external fun bridgeListen(callback: (String) -> Unit)

/**
 * `postMessage` 的请求 / 回包封装。
 *
 * 每次调用生成自增 id，回包按 id 找回等待中的协程；5s 没回包就失败，
 * 避免 UI 永久停在加载态。
 */
class Bridge(private val timeoutMs: Long = 5000) {

    private val json = Json { ignoreUnknownKeys = true }
    private val pending = mutableMapOf<Int, CompletableDeferred<JsonElement>>()
    private var sequence = 0
    private var listening = false

    private fun ensureListening() {
        if (listening) return
        bridgeListen(::onMessage)
        listening = true
    }

    private fun onMessage(text: String) {
        val envelope = runCatching { json.parseToJsonElement(text).jsonObject }.getOrNull() ?: return
        val id = envelope["id"]?.jsonPrimitive?.intOrNull ?: return
        val deferred = pending.remove(id) ?: return
        val ok = envelope["ok"]?.jsonPrimitive?.booleanOrNull ?: false
        if (ok) {
            deferred.complete(envelope["result"] ?: JsonNull)
        } else {
            val error = envelope["error"]?.jsonObject
            val code = error?.get("code")?.jsonPrimitive?.intOrNull ?: -1
            val message = error?.get("message")?.jsonPrimitive?.contentOrNull ?: "桥调用失败（$code）"
            deferred.completeExceptionally(BridgeException(code, message))
        }
    }

    suspend fun call(
        method: String,
        params: JsonObject = JsonObject(emptyMap()),
        timeoutMs: Long = this.timeoutMs,
    ): JsonElement {
        if (!available()) throw BridgeUnavailableException()
        ensureListening()

        val id = ++sequence
        val deferred = CompletableDeferred<JsonElement>()
        pending[id] = deferred
        val request = buildJsonObject {
            put("id", id)
            put("method", method)
            put("params", params)
        }
        bridgePost(request.toString())
        return try {
            withTimeout(timeoutMs) { deferred.await() }
        } finally {
            pending.remove(id)
        }
    }

    companion object {
        fun available(): Boolean = bridgeAvailable()
    }
}
