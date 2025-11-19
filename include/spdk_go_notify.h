#ifndef SPDK_GO_NOTIFY_H
#define SPDK_GO_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Send an event notification to the local web backend.
 * This function automatically initializes the notifier on first call.
 * The notification is sent asynchronously and does not block.
 *
 * @param event   event name (non-empty string)
 * @param payload JSON string representing the event payload, or NULL/empty for no payload
 * @return 0 on success (notification queued), negative value on failure
 *
 * Example:
 *   NotifyEvent("bdev_ready", "{\"name\":\"nvme0n1\"}");
 *   NotifyEvent("error_occurred", NULL);
 */
int NotifyEvent(const char *event, const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_GO_NOTIFY_H */

