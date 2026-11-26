#pragma once
#include <stddef.h>
#include <string>
#include <string_view>
#include "http_server.h"

namespace fortis::operations {
/** Public read-only deployment views. Directory is copied at registration;
 * register once before workers start. No controller commands or KV access.
 * File reads are bounded, regular-file-only and memoized for one second.
 * Missing metadata reports unmanaged; corruption/unreadable snapshots return
 * a fixed 503 without paths or contents. Guest builds need no host facilities.
 */
bool RegisterReleaseRoutes(http::HttpRouter& router, std::string_view directory);

/** The host's own pages. The host orchestrator, not this node, is the author
 * of what the address `/` and `/health` show: it writes finished files into
 * the read-only directory given to RegisterReleaseRoutes, and every node
 * release hands them out unchanged. This is a fixed contract between the two:
 *
 *   index.html     shown at /             (ServeHostPage; the node's built-in
 *                                          home page is the fallback)
 *   health.html    shown at /health
 *   health.json    shown at /health.json
 *   health.status  one line "STATUS WRITTEN_MS": the status code to answer
 *                  with, and when the report was written
 *   node.json      shown at /node.json: which release runs, which is coming
 *                  and when (404 when the host writes none)
 *
 * Copies the file `name` into `out`. False when there is no such file (an
 * unmanaged host) or it is not a regular file of at most 512 KiB. Reads are
 * bounded and never follow a link. Safe to call from any worker. */
bool CopyHostPage(const char* name, std::string* out);
/** The status the host's health report states, or 503 when the report is
 * missing, unreadable, or older than a minute: a report nobody refreshes is
 * not believed. */
int HostHealthStatus();
}  // namespace fortis::operations
