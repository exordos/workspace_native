"""Conversation and call responses for the installed-desktop loopback acceptance."""
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlparse
from uuid import uuid4


class PackageConversation:
    def __init__(self, project, user, origin):
        self.project = project
        self.user = user
        self.origin = origin
        self.stream = str(uuid4())
        self.topic = str(uuid4())
        self.message = str(uuid4())
        self.meeting_message = str(uuid4())
        self.target_content = "Cassi installed desktop target " + uuid4().hex[:10]
        self.stream_name = "Cassi hosted channel"
        self.topic_name = "Cassi installed desktop checks"
        self.meeting_url = origin + "/hosted-desktop-call"
        self.timestamp = datetime.now(timezone.utc).isoformat()
        self.generation = str(uuid4())
        self.target_gets = 0
        self.history_gets = 0
        self.probe_results = {}
        self.routes = set()

    def row(self, message_id, content):
        return {"uuid": message_id, "project_id": self.project, "stream_uuid": self.stream,
                "topic_uuid": self.topic, "user_uuid": self.user, "author_uuid": self.user,
                "is_own": True, "read": True, "starred": False, "created_at": self.timestamp,
                "payload": {"kind": "markdown", "content": content}}

    def handle(self, request):
        parsed = urlparse(request.path)
        path = parsed.path.removeprefix("/api/workspace/v1/messenger/")
        self.routes.add((path.replace(self.stream, "<stream>").replace(self.message, "<message>").replace(self.topic, "<topic>"), tuple(sorted(parse_qs(parsed.query)))))

        if parsed.path == "/hosted-desktop-call":
            page = b"""<!doctype html><meta charset=utf-8><title>Hosted media check</title>
<script>
(async () => {
  const report = (kind, result) => fetch("/hosted-media-result?" + new URLSearchParams({kind, result}));
  await report("page", "loaded");
  for (const kind of ["audio", "video"]) {
    try {
      const stream = await navigator.mediaDevices.getUserMedia({[kind]: true});
      await report(kind, stream.getTracks().some(t => t.readyState === "live") ? "live" : "ended");
      stream.getTracks().forEach(t => t.stop());
    } catch (error) { await report(kind, error.name); }
  }
})()
</script>"""
            request.send_response(200)
            request.send_header("Content-Type", "text/html; charset=utf-8")
            request.send_header("Content-Length", str(len(page)))
            request.end_headers()
            request.wfile.write(page)
            return True
        if parsed.path == "/hosted-media-result":
            query = parse_qs(parsed.query)
            kind = query.get("kind", [""])[0]
            result = query.get("result", [""])[0]
            if kind in ("page", "audio", "video") and result in ("loaded", "live", "ended", "NotFoundError", "NotAllowedError", "NotReadableError"):
                self.probe_results[kind] = result
            request.reply(200, {})
            return True
        if parsed.path == "/api/workspace/v1/epoch/":
            request.reply(200, {"epoch_generation": self.generation, "epoch_version": 1,
                                "current_epoch_version": 1, "minimum_epoch_version": 1})
            return True
        if path == "server_settings":
            request.server.settings_count += 1
            request.reply(200, {"realm_name": "Cassi package fixture", "meet_url": self.origin})
            return True
        if request.headers.get("Authorization") != "Bearer " + request.server.access:
            return False
        shared = {"project_id": self.project, "user_uuid": self.user, "created_at": self.timestamp,
                  "updated_at": self.timestamp, "notification_mode": "default", "unread_count": 0,
                  "active_unread_count": 0, "passive_unread_count": 0, "last_message_uuid": self.message}
        stream = dict(shared, uuid=self.stream, name=self.stream_name, owner=self.user, role="owner",
                      description="", source_name="native", notification_mode="all_messages", invite_only=False, announce=False, private=False,
                      current_encryption_key_uuid=None, direct_user_uuid=None, provider_uuid=None, provider_identity=None,
                      is_archived=False, encryption=False, default_topic_uuid=self.topic)
        topic = dict(shared, uuid=self.topic, stream_uuid=self.stream, name=self.topic_name, is_default=True, is_done=False)
        target = self.row(self.message, self.target_content)
        call = self.row(self.meeting_message, self.meeting_url)
        if path == "streams/": payload = [stream]
        elif path == "streams/" + self.stream: payload = stream
        elif path == "stream_topics/": payload = [topic]
        elif path == "stream_topics/" + self.topic: payload = topic
        elif path == "messages/" and parse_qs(parsed.query).get("uuid") == [self.message]:
            self.target_gets += 1
            payload = [target]
        elif path == "messages/" + self.message: payload = target
        elif path == "messages/" + self.meeting_message: payload = call
        elif path == "messages/" and ("stream_uuid" in parse_qs(parsed.query) or "topic_uuid" in parse_qs(parsed.query)):
            self.history_gets += 1
            payload = [target, call]
        else: return False
        request.reply(200, payload)
        return True
