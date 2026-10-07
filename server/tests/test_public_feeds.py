"""Public-source filtering, history retention, and temporary-origin failures."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from public_feeds import PublicFeedAdapter, PublicFeedError, build_rss, podcast_from_page

FIXTURES = Path(__file__).parent / "fixtures"
LI = "65bb55f6513a776b57dedb32"
WX = "65f02690587b754dbe358a7d"


def fixture(name):
    return json.loads((FIXTURES / name).read_text())


def page(podcast):
    data = {"props": {"pageProps": {"podcast": podcast}}}
    return ('<html><script id="__NEXT_DATA__" type="application/json">'
            + json.dumps(data, ensure_ascii=False) + '</script></html>').encode()


class PublicFeedTests(unittest.TestCase):
    def setUp(self):
        self.li = fixture("lidan-public-page.json")
        self.wx = fixture("wechat-talk-public-page.json")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.adapter = PublicFeedAdapter(self.temp.name, {}, ttl=0)

    def test_real_public_samples_have_own_recent_episodes(self):
        for pid, podcast, count, title in (
            (LI, self.li, 13, "李诞"), (WX, self.wx, 10, "微信公开TALK")
        ):
            parsed = podcast_from_page(page(podcast).decode(), pid)
            channel = ET.fromstring(build_rss(parsed, pid)).find("channel")
            self.assertEqual(channel.findtext("title"), title)
            self.assertEqual(len(channel.findall("item")), count)
            self.assertEqual(channel.findtext("item/title"), podcast["episodes"][0]["title"])
            for item in channel.findall("item"):
                self.assertTrue(item.find("enclosure").get("url").startswith("https://media.xyzcdn.net/"))

    def test_other_show_paid_private_preview_and_changed_media_are_excluded(self):
        original = self.wx["episodes"][0]
        invalid = []
        for field, value in (("pid", LI), ("payType", "PAID"), ("status", "DELETED"),
                             ("isPrivateMedia", True), ("isTrial", True), ("isPreview", True)):
            episode = copy.deepcopy(original)
            episode[field] = value
            invalid.append(episode)
        for field, value in (("mode", "PRIVATE"), ("url", "https://example.invalid/media.mp3")):
            episode = copy.deepcopy(original)
            episode["media"]["source"][field] = value
            invalid.append(episode)
        podcast = copy.deepcopy(self.wx)
        podcast["episodes"] = [original] + invalid
        self.assertEqual(len(ET.fromstring(build_rss(podcast, WX)).findall("channel/item")), 1)

    def test_malformed_episode_fields_do_not_hide_other_valid_episodes(self):
        podcast = copy.deepcopy(self.wx)
        bad = []
        for field, value in (("media", "wrong"), ("enclosure", []), ("pubDate", 1),
                             ("eid", "wrong"), ("duration", "wrong")):
            episode = copy.deepcopy(podcast["episodes"][0])
            episode[field] = value
            bad.append(episode)
        podcast["episodes"].extend(bad)
        self.assertEqual(len(ET.fromstring(build_rss(podcast, WX)).findall("channel/item")), 10)

    def test_page_identity_and_changed_schema_fail_explicitly(self):
        for html in ("<html>empty</html>", page(self.wx).decode()):
            with self.assertRaises(PublicFeedError):
                podcast_from_page(html, LI)

    def test_native_history_is_merged_without_repeated_recent_episode(self):
        legacy = ET.fromstring(build_rss(self.li, LI))
        old = copy.deepcopy(legacy.find("channel/item"))
        old.find("title").text = "历史保留单集"
        old.find("link").text = "https://www.xiaoyuzhoufm.com/episode/111111111111111111111111"
        old.find("guid").text = old.findtext("link")
        old.find("pubDate").text = "Thu, 09 Jan 2020 00:00:00 GMT"
        old.find("enclosure").set("url", "https://media.xyzcdn.net/history.m4a")
        legacy.find("channel").append(old)
        channel = ET.fromstring(build_rss(self.li, LI, ET.tostring(legacy))).find("channel")
        self.assertEqual(len(channel.findall("item")), 14)
        self.assertEqual(channel.findall("item")[-1].findtext("title"), "历史保留单集")
        legacy.find("channel/title").text = "其他节目"
        with self.assertRaises(PublicFeedError):
            build_rss(self.li, LI, ET.tostring(legacy))

    def test_no_history_source_on_first_fetch_does_not_publish_partial_history(self):
        with mock.patch.object(self.adapter, "_read", side_effect=[page(self.li), OSError("offline")]):
            with self.assertRaises(OSError):
                self.adapter.get(LI)
        self.assertFalse((Path(self.temp.name) / (LI + ".json")).exists())

    def test_native_tracking_urls_and_guid_ids_preserve_history_and_deduplicate(self):
        legacy = ET.fromstring(build_rss(self.li, LI))
        for item in legacy.findall("channel/item"):
            eid = item.findtext("link").rsplit("/", 1)[1]
            item.find("guid").text = eid
            item.find("link").text += "?utm_source=rss"
            direct = item.find("enclosure").get("url")
            item.find("enclosure").set("url", "https://dts-api.xiaoyuzhoufm.com/track/" + LI + "/" + eid + "/" + direct.removeprefix("https://"))
        old = copy.deepcopy(legacy.find("channel/item"))
        eid = "111111111111111111111111"
        old.find("guid").text = eid
        old.find("link").text = "https://www.xiaoyuzhoufm.com/episode/" + eid + "?utm_source=rss"
        old.find("enclosure").set("url", "https://dts-api.xiaoyuzhoufm.com/track/" + LI + "/" + eid + "/media.xyzcdn.net/history.m4a")
        legacy.find("channel").append(old)
        self.assertEqual(len(ET.fromstring(build_rss(self.li, LI, ET.tostring(legacy))).findall("channel/item")), 14)
        old.find("enclosure").set("url", old.find("enclosure").get("url").replace("/track/" + LI + "/", "/track/" + WX + "/"))
        self.assertEqual(len(ET.fromstring(build_rss(self.li, LI, ET.tostring(legacy))).findall("channel/item")), 13)

    def test_origin_failure_retains_cached_feed_and_marks_it_stale(self):
        with mock.patch.object(self.adapter, "_read", return_value=page(self.wx)):
            first, stale = self.adapter.get(WX)
        self.assertFalse(stale)
        cache = Path(self.temp.name) / (WX + ".json")
        before = cache.read_bytes()
        with mock.patch.object(self.adapter, "_read", side_effect=OSError("offline")):
            second, stale = self.adapter.get(WX)
        self.assertEqual(first, second)
        self.assertTrue(stale)
        self.assertEqual(cache.read_bytes(), before)
        self.assertEqual(cache.stat().st_mode & 0o777, 0o600)

    def test_fresh_cache_avoids_origin_request(self):
        self.adapter.ttl = 300
        with mock.patch.object(self.adapter, "_read", return_value=page(self.wx)) as read:
            first = self.adapter.get(WX)
            self.assertEqual(first, self.adapter.get(WX))
        self.assertEqual(read.call_count, 1)

    def test_history_origin_failure_reuses_known_history(self):
        with mock.patch.object(self.adapter, "_read", side_effect=[page(self.li), build_rss(self.li, LI)]):
            first, _ = self.adapter.get(LI)
        newer = copy.deepcopy(self.li)
        newer["episodes"] = [newer["episodes"][0]]
        with mock.patch.object(self.adapter, "_read", side_effect=[page(newer), OSError("offline")]):
            result, _ = self.adapter.get(LI)
        self.assertEqual(len(ET.fromstring(result).findall("channel/item")), 13)

    def test_later_public_window_keeps_previous_known_episodes(self):
        with mock.patch.object(self.adapter, "_read", return_value=page(self.wx)):
            self.adapter.get(WX)
        newer = copy.deepcopy(self.wx)
        newer["episodes"] = [newer["episodes"][0]]
        with mock.patch.object(self.adapter, "_read", return_value=page(newer)):
            result, _ = self.adapter.get(WX)
        self.assertEqual(len(ET.fromstring(result).findall("channel/item")), 10)

    def test_history_does_not_reintroduce_current_paid_private_or_foreign_episodes(self):
        prior = build_rss(self.wx, WX)
        for field, value in (("payType", "PAID"), ("pid", LI), ("isPrivateMedia", True),
                             ("isPreview", True), ("status", "DELETED")):
            current = copy.deepcopy(self.wx)
            current["episodes"][0][field] = value
            merged = ET.fromstring(build_rss(current, WX, prior)).findall("channel/item")
            self.assertEqual(len(merged), 9)
            self.assertNotIn(current["episodes"][0]["title"], [item.findtext("title") for item in merged])

    def test_all_known_episodes_becoming_paid_does_not_return_old_public_cache(self):
        with mock.patch.object(self.adapter, "_read", return_value=page(self.wx)):
            self.adapter.get(WX)
        current = copy.deepcopy(self.wx)
        for episode in current["episodes"]:
            episode["payType"] = "PAID"
        with mock.patch.object(self.adapter, "_read", return_value=page(current)):
            result, stale = self.adapter.get(WX)
        self.assertFalse(stale)
        self.assertEqual(ET.fromstring(result).findall("channel/item"), [])

    def test_corrupted_cache_rebuilds_instead_of_crashing(self):
        cache = Path(self.temp.name) / (WX + ".json")
        for content in ("not json", "[]", '{"updated":"wrong","xml":17}'):
            cache.write_text(content)
            with mock.patch.object(self.adapter, "_read", return_value=page(self.wx)):
                self.assertFalse(self.adapter.get(WX)[1])

    def test_unconfigured_program_never_makes_network_request(self):
        with mock.patch.object(self.adapter, "_read", side_effect=AssertionError("network")):
            with self.assertRaises(PublicFeedError):
                self.adapter.get("unknown")
