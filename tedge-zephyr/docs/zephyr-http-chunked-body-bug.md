# Zephyr's HTTP client mis-reports chunked response bodies

A report prepared for upstream Zephyr (`zephyrproject-rtos/zephyr`). It is
kept here because the client works around the bug, and the workaround should
be removed once it is fixed.

- **Affects:** Zephyr 4.4.2 (seen), `subsys/net/lib/http/http_client.c`
- **Severity:** silent data corruption for any chunked download
- **Workaround in this module:** `src/tedge_http_download.c` writes every
  byte from the parser's `on_body` callback and ignores
  `rsp->body_frag_start` / `rsp->body_frag_len`.

## What happens

With `Transfer-Encoding: chunked`, one receive buffer often holds several
body segments separated by chunk-size lines:

```
<chunk-size>\r\n <segment A> \r\n <chunk-size>\r\n <segment B> \r\n ...
```

The HTTP client calls the parser, which reports each segment through
`on_body`, and the client records the segment in the `http_response` it
passes to the application's response callback. It keeps **the first
segment's `body_frag_start`** but overwrites **`body_frag_len` with the last
segment's length**. An application that copies
`body_frag_start[0 .. body_frag_len)` therefore stores the wrong bytes: it
reads from the first segment's offset for the length of the last one,
including the chunk-size lines that separate them.

## How it showed up

Downloading a ~900 KB firmware image from Cumulocity IoT, which serves
binaries chunked, into an MCUboot slot on an ESP32-C6. The stored image was
corrupt in a way that depended on how the segments fell across receive
buffers. MCUboot rejected it at the signature check and booted the previous
image, so the failure was safe but silent until the image was inspected.

## Reproduction

1. Serve a file larger than the receive buffer with
   `Transfer-Encoding: chunked` (any HTTP server; Cumulocity's
   `/inventory/binaries/<id>` does this).
2. Fetch it with `http_client_req()` and a `recv_buf` of 1 KB.
3. In the response callback, append `body_frag_start[0 .. body_frag_len)` to
   a file for every call.
4. Compare with the original: the copy differs, and usually contains ASCII
   chunk-size lines.

Fetching the same file with `Content-Length` instead of chunked encoding
produces a correct copy, which isolates the chunked path.

## Suggested fix

Report each body segment separately (call the response callback per
segment), or accumulate the segments so that `body_frag_start` and
`body_frag_len` describe one contiguous run. Until then, the
parser's `on_body` callback is the only reliable source of body bytes, and
that is worth saying in the API documentation.
