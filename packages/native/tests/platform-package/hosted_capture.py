"""Local meeting and capture observations for the installed desktop checks."""
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlparse
from uuid import uuid4


PAGE = b"""<!doctype html><meta charset=utf-8><title>Workspace capture check</title>
<script>
(async () => {
    const report = (kind, result) =>
        fetch("/capture-result?" + new URLSearchParams({kind, result}));
    await report("page", "loaded");
    for (const kind of ["audio", "video"]) {
        try {
            const stream = await navigator.mediaDevices.getUserMedia({[kind]: true});
            const live = stream.getTracks().some(track => track.readyState === "live");
            stream.getTracks().forEach(track => track.stop());
            await report(kind, live ? "live" : "ended");
        } catch (error) {
            await report(kind, error.name);
        }
    }
})()
</script>"""


class HostedCapture:
    def __init__(self, project, user, origin):
        self.project = project
        self.user = user
        self.origin = origin
        self.stream = str(uuid4())
        self.topic = str(uuid4())
        self.message = str(uuid4())
        self.generation = str(uuid4())
        self.timestamp = datetime.now(timezone.utc).isoformat()
        self.results = {}

    def handle(self, request):
        url = urlparse(request.path)
        if url.path == "/hosted-desktop-call":
            request.send_response(200)
            request.send_header("Content-Type", "text/html; charset=utf-8")
            request.send_header("Content-Length", str(len(PAGE)))
            request.end_headers()
            request.wfile.write(PAGE)
            return True
        if url.path == "/capture-result":
            query = parse_qs(url.query)
            kind = query.get("kind", [""])[0]
            if kind in ("page", "audio", "video"):
                self.results[kind] = query.get("result", [""])[0]
            request.reply(200, {})
            return True
        if request.headers.get("Authorization") != "Bearer " + request.server.access:
            return False
        if url.path == "/api/workspace/v1/epoch/":
            request.reply(200, {"epoch_generation": self.generation, "epoch_version": 1,
                                "current_epoch_version": 1, "minimum_epoch_version": 1})
            return True
        prefix = "/api/workspace/v1/messenger/"
        if not url.path.startswith(prefix):
            return False
        path = url.path[len(prefix):]
        shared = {"project_id": self.project, "user_uuid": self.user, "created_at": self.timestamp,
                  "updated_at": self.timestamp, "unread_count": 0, "active_unread_count": 0,
                  "passive_unread_count": 0, "last_message_uuid": self.message}
        if path in ("streams/", "streams/" + self.stream):
            stream = dict(shared, uuid=self.stream, name="Cassi hosted channel", owner=self.user,
                          role="owner", description="", source_name="native", notification_mode="all_messages",
                          invite_only=False, announce=False, private=False, is_archived=False, encryption=False,
                          current_encryption_key_uuid=None, direct_user_uuid=None, provider_uuid=None,
                          provider_identity=None, default_topic_uuid=self.topic)
            payload = [stream] if path == "streams/" else stream
        elif path in ("stream_topics/", "stream_topics/" + self.topic):
            topic = dict(shared, uuid=self.topic, stream_uuid=self.stream, name="Hosted call",
                         is_default=True, is_done=False, notification_mode="default")
            payload = [topic] if path == "stream_topics/" else topic
        elif path == "messages/" and parse_qs(url.query).get("stream_uuid") == [self.stream]:
            payload = [{"uuid": self.message, "project_id": self.project, "stream_uuid": self.stream,
                        "topic_uuid": self.topic, "user_uuid": self.user, "author_uuid": self.user,
                        "is_own": True, "read": True, "starred": False, "created_at": self.timestamp,
                        "payload": {"kind": "markdown", "content": self.origin + "/hosted-desktop-call"}}]
        else:
            return False
        request.reply(200, payload)
        return True
