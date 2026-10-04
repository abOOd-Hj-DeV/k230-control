#!/usr/bin/env python3
"""Reproducible, public synthetic metadata only. No media or private keys."""
import hashlib
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "protocol" / "v2"
CHECK = False
UUID = {"type": "string", "pattern": "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}
DECIMAL = {"type": "string", "pattern": "^(0|[1-9][0-9]{0,18})$", "maxLength": 19}
POSITIVE = {"type": "string", "pattern": "^[1-9][0-9]{0,18}$", "maxLength": 19}
BOOL = {"type": "boolean"}
NULL = {"type": "null"}
PROB = {"type": "number", "minimum": 0, "maximum": 1}
ERRORS = "unsupported_version malformed_json bounds invalid_age policy_mismatch session_mismatch stream_mismatch stale future_pts clock_unverified wrong_screen invalid_transform invalid_scores invalid_evidence hentai_stage3_forbidden capability_missing permission_missing locked action_failed home_unverified event_conflict rate_limited busy storage_failed".split()


def enum(*values):
    return {"enum": list(values)}


def integer(lo, hi):
    return {"type": "integer", "minimum": lo, "maximum": hi}


def obj(**properties):
    return {"type": "object", "properties": properties, "required": list(properties), "additionalProperties": False}


def arr(items, lo=0, hi=32):
    return {"type": "array", "items": items, "minItems": lo, "maxItems": hi}


def nullable(schema):
    return {"anyOf": [NULL, schema]}


PACKAGE = {"type": "string", "maxLength": 255, "pattern": "^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*$"}
RECT = obj(x=integer(0, 16384), y=integer(0, 16384), width=integer(1, 16384), height=integer(1, 16384))
ROTATION = {"type": "integer", "enum": [0, 90, 180, 270]}
POLICY = obj(policy_version=enum("age-10-15-v1"), policy_revision=POSITIVE, age=integer(10, 15), profile=enum("10-12", "13-15"))
POLICY["allOf"] = [{"if": {"properties": {"age": {"maximum": 12}}}, "then": {"properties": {"profile": enum("10-12")}}, "else": {"properties": {"profile": enum("13-15")}}}]
SCREEN = obj(screen_token=UUID, content_epoch=POSITIVE, display_id=enum(0), width=integer(1, 16384), height=integer(1, 16384), rotation_deg=ROTATION, window_id=integer(-1, 2147483647), package={"anyOf": [PACKAGE, enum("")]}, sampled_at_us=DECIMAL, valid_from_us=DECIMAL, status=enum("verified", "invalid", "locked", "unsupported"))
SCREEN["allOf"] = [{"if": {"properties": {"status": enum("verified")}}, "then": {"properties": {"package": PACKAGE, "window_id": integer(0, 2147483647)}}}]
SCORES = obj(porn=PROB, hentai=PROB, sexy=PROB, explicit_score=PROB)
OBS = obj(pts_us=DECIMAL, porn=PROB, hentai=PROB, sexy=PROB)
EVIDENCE = obj(route=enum("explicit", "hentai_dominant"), observations=arr(OBS, 3), analysis_continuity_id=UUID, analysis_complete=enum(True))
REGION = obj(track_id=POSITIVE, kind=enum("Image", "BackgroundImage", "Video"), crop_frame_px=RECT, scores=SCORES, evidence=EVIDENCE)
COMMON = dict(v=enum(2), type={}, session_id=UUID, seq=POSITIVE, stream_id=UUID)


def message(name, **fields):
    return obj(**{**COMMON, "type": enum(name), **fields})


HELLO = obj(v=enum(2), type=enum("hello"), session_id=UUID, phone_boot_id=UUID, versions=arr(enum(2), 1, 1), max_line_bytes=enum(16384), phone_time_us=DECIMAL, clock=enum("android_system_nano_time_us"), capabilities={**arr(enum("cover_region", "calm_shield", "home"), 0, 3), "uniqueItems": True}, policy=nullable(POLICY), screen=nullable(SCREEN))
BIND = message("bind", pts_clock=enum("android_system_nano_time_us"), capture_pts_us=nullable(DECIMAL), capture=obj(source=enum("scrcpy-4.0-display", "android-mediaprojection-display"), display_id=enum(0), mirror=enum(False), custom_crop=enum(False), custom_rotation=enum(False)))
BOUND = message("bound", request_seq=POSITIVE, status=enum("pending", "accepted", "rejected"), error=nullable(enum(*ERRORS)), phone_time_us=DECIMAL)
BOUND["allOf"] = [{"if": {"properties": {"status": enum("rejected")}}, "then": {"properties": {"error": enum(*ERRORS)}}, "else": {"properties": {"error": NULL}}}]
STATE = message("state", phone_time_us=DECIMAL, policy=nullable(POLICY), screen=nullable(SCREEN), protection=obj(stage=integer(0, 3), event_id=nullable(UUID), action_revision=nullable(POSITIVE), target_screen_token=nullable(UUID), applied_at_us=nullable(DECIMAL), covered_rects=arr(RECT, 0, 8), release_pending=BOOL), health=obj(accessibility=BOOL, keystore=enum("ready", "locked", "failed"), pairing=enum("paired", "unpaired", "revoked", "key_lost"), outbox_count=integer(0, 10000), cloud=enum("online", "offline", "unconfigured", "auth_error")))
STATE["properties"]["protection"]["allOf"] = [{"if": {"properties": {"stage": enum(0)}}, "then": {"properties": {"event_id": NULL, "action_revision": NULL, "target_screen_token": NULL, "applied_at_us": NULL, "covered_rects": arr(RECT, 0, 0)}}, "else": {"properties": {"event_id": UUID, "action_revision": POSITIVE, "target_screen_token": UUID, "applied_at_us": DECIMAL}}}]
DECISION = message("decision", event_id=UUID, action_revision=POSITIVE, pts_us=DECIMAL, expires_at_us=DECIMAL, screen_token=UUID, content_epoch=POSITIVE, package=PACKAGE, window_id=integer(0, 2147483647), policy=POLICY, frame=obj(width=integer(1, 16384), height=integer(1, 16384), display_rotation_deg=ROTATION), transform=obj(rotation_cw_deg=enum(0), viewport_display_px=RECT), requested_stage=integer(1, 3), requested_action=enum("cover_region", "calm_shield", "home"), reason=enum("threshold", "repetition"), regions=arr(REGION, 1, 8))
DECISION["allOf"] = [
    {"if": {"properties": {"requested_stage": enum(stage)}}, "then": {"properties": {"requested_action": enum(action), "regions": arr(REGION, 1, 8 if stage == 1 else 1)}}}
    for stage, action in [(1, "cover_region"), (2, "calm_shield"), (3, "home")]
] + [{"if": {"properties": {"reason": enum("repetition")}}, "then": {"properties": {"requested_stage": enum(2)}}}]
ACK = message("ack", request_seq=POSITIVE, event_id=UUID, action_revision=POSITIVE, status=enum("executed", "duplicate", "rejected", "failed", "pending"), requested_stage=integer(1, 3), executed_stage=integer(0, 3), executed_action=enum("none", "cover_region", "calm_shield", "home"), executed_at_us=nullable(DECIMAL), screen_token=UUID, display_rects=arr(RECT, 0, 8), error=nullable(enum(*ERRORS)))
RELEASED = message("released", event_id=UUID, action_revision=POSITIVE, reason=enum("verified_navigation", "verified_new_content", "guardian_grant"), released_at_us=DECIMAL, previous_screen_token=UUID, new_screen_token=nullable(UUID))


def uid(n):
    return f"10000000-0000-4000-8000-{n:012d}"


def rect(x, y, w, h):
    return dict(x=x, y=y, width=w, height=h)


def write(path, data):
    contents = json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    if CHECK:
        if not path.exists() or path.read_text() != contents:
            raise ValueError(f"fixture is not reproducible: {path.relative_to(ROOT)}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def generate():
    write(ROOT / "companion.schema.json", {"$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:mentor:parental:v2:companion", "title": "mentor-parental-v2.0", "oneOf": [HELLO, BIND, BOUND, STATE, DECISION, ACK, RELEASED]})
    profile = dict(policy_version="age-10-15-v1", policy_revision="1", age=12, profile="10-12")
    screen = dict(screen_token=uid(2), content_epoch="1", display_id=0, width=1080, height=2400, rotation_deg=0, window_id=7, package="com.example.viewer", sampled_at_us="10400000", valid_from_us="9900000", status="verified")
    base = dict(v=2, session_id=uid(1), stream_id=uid(4))
    hello = dict(v=2, type="hello", session_id=uid(1), phone_boot_id=uid(3), versions=[2], max_line_bytes=16384, phone_time_us="10000000", clock="android_system_nano_time_us", capabilities=["cover_region", "calm_shield", "home"], policy=profile, screen=screen)
    bind = dict(**base, type="bind", seq="1", pts_clock="android_system_nano_time_us", capture_pts_us=None, capture=dict(source="scrcpy-4.0-display", display_id=0, mirror=False, custom_crop=False, custom_rotation=False))
    bound = dict(**base, type="bound", seq="1", request_seq="1", status="accepted", error=None, phone_time_us="10010000")
    state = dict(**base, type="state", seq="2", phone_time_us="10400000", policy=profile, screen=screen, protection=dict(stage=0, event_id=None, action_revision=None, target_screen_token=None, applied_at_us=None, covered_rects=[], release_pending=False), health=dict(accessibility=True, keystore="ready", pairing="paired", outbox_count=0, cloud="offline"))
    probabilities = dict(porn=.65, hentai=.02, sexy=.10)
    evidence = dict(route="explicit", observations=[dict(pts_us=str(t), **probabilities) for t in [10000000, 10200000, 10400000]], analysis_continuity_id=uid(6), analysis_complete=True)
    decision = dict(**base, type="decision", seq="3", event_id=uid(5), action_revision="1", pts_us="10400000", expires_at_us="11150000", screen_token=uid(2), content_epoch="1", package="com.example.viewer", window_id=7, policy=profile, frame=dict(width=360, height=800, display_rotation_deg=0), transform=dict(rotation_cw_deg=0, viewport_display_px=rect(0, 0, 1080, 2400)), requested_stage=1, requested_action="cover_region", reason="threshold", regions=[dict(track_id="1", kind="Image", crop_frame_px=rect(20, 100, 160, 300), scores=dict(**probabilities, explicit_score=.67), evidence=evidence)])
    ack = dict(**base, type="ack", seq="3", request_seq="3", event_id=uid(5), action_revision="1", status="executed", requested_stage=1, executed_stage=1, executed_action="cover_region", executed_at_us="10450000", screen_token=uid(2), display_rects=[rect(60, 300, 480, 900)], error=None)
    released = dict(**base, type="released", seq="4", event_id=uid(5), action_revision="1", reason="verified_navigation", released_at_us="12000000", previous_screen_token=uid(2), new_screen_token=uid(7))
    for record in [hello, bind, bound, state, decision, ack, released]:
        write(ROOT / "fixtures" / f"{record['type']}.json", record)
    traces = []
    for age in [10, 12, 13, 15]:
        cover, shield, exit_score = (.60, .80, .90) if age <= 12 else (.70, .85, .95)
        for name, p, h, s, count, spacing, stage in [
            ("sexy_only", 0, 0, 1, 11, 100000, 0),
            ("cover_inclusive", cover, 0, 0, 3, 200000, 1),
            ("cover_too_short", cover, 0, 0, 3, 100000, 0),
            ("shield_inclusive", shield, 0, 0, 3, 200000, 2),
            ("exit_first_action", exit_score, 0, 0, 11, 100000, 3),
            ("exit_five_too_short", exit_score, 0, 0, 5, 100000, 0),
            ("hentai_strict_boundary", 0, .60, 0, 11, 100000, 0),
            ("hentai_shield_never_home", .01, .98, 0, 11, 100000, 2),
            ("low_porn_cannot_home", .599999, .4, 0, 11, 100000, 2),
            ("porn_point_six_can_home", .60, .39, 0, 11, 100000, 3),
        ]:
            traces.append(dict(name=f"{age}_{name}", age=age, porn=p, hentai=h, sexy=s, count=count, spacing_us=spacing, expected_stage=stage))
    write(ROOT / "fixtures" / "policy-traces.json", dict(contract="mentor-parental-v2.0", cases=traces))
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(ROOT.rglob("*.json")) if p.name != "manifest.json"}
    write(ROOT / "manifest.json", dict(contract="mentor-parental-v2.0", sha256=hashes))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    CHECK = parser.parse_args().check
    generate()
