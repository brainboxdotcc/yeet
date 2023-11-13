#include <typeinfo>
#include <dpp/dpp.h>
#include <beholder/beholder.h>
#include <beholder/database.h>
#include <dpp/json.h>
#include <beholder/whitelist.h>
#include <CxxUrl/url.hpp>
#include <beholder/sentry.h>

std::atomic<int> concurrent_images{0};

void download_image(const dpp::attachment attach, dpp::cluster& bot, const dpp::message_create_t ev) {
	std::string lower_url = dpp::lowercase(attach.url);
	std::string path;
	try {
		Url u(lower_url);
		path = u.path();
	}
	catch (const std::exception& e) {
		return;
	}
	if (path.ends_with(".webp") || path.ends_with(".jpg") || path.ends_with(".jpeg") || path.ends_with(".png") || path.ends_with(".gif")) {
		bot.log(dpp::ll_info, "Download image: " + path);
		if (concurrent_images > max_concurrency) {
			bot.log(dpp::ll_info, "Too many concurrent images, skipped");
			return;
		}
		for (int index = 0; whitelist[index] != nullptr; ++index) {
			if (match(attach.url.c_str(), whitelist[index])) {
				bot.log(dpp::ll_info, "Image " + attach.url + " is whitelisted by " + std::string(whitelist[index]) + "; not scanning");
				return;
			}
		}
		/**
		 * NOTE: The width, height and size attributes given here are only valid if the image was uploaded as
		 * an attachment. If the image we are processing came from a URL these can't be filled yet, and will
		 * be checked after we have downloaded the image. Bandwidth is cheap, so this doesnt matter too much,
		 * it's just the processing cost of running OCR on a massive image we would want to prevent.
		 */
		if (attach.width * attach.height > 33554432) {
			bot.log(dpp::ll_info, "Image dimensions of " + std::to_string(attach.width) + "x" + std::to_string(attach.height) + " too large to be a screenshot");
			return;
		}
		db::resultset pattern_count = db::query("SELECT COUNT(guild_id) AS total FROM guild_patterns WHERE guild_id = ?", { ev.msg.guild_id.str() });
		if (pattern_count.size() == 0 || atoi(pattern_count[0].at("total").c_str()) == 0) {
			bot.log(dpp::ll_info, "No patterns defined for guild " + ev.msg.guild_id.str());
			return;
		}
		std::string url_front = attach.url;
		bot.request(attach.url, dpp::m_get, [attach, ev, &bot](const dpp::http_request_completion_t& result) {
			/**
			 * Check size of downloaded file again here, because an attachment gives us the size
			 * before we try to download it, a url does not. 
			 */
			concurrent_images++;
			std::thread hard_work(ocr::image, result.body, attach, std::ref(bot), ev);
			hard_work.detach();
		});
	}
}

