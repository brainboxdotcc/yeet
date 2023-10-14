#include <dpp/dpp.h>
#include <yeet/yeet.h>
#include <yeet/database.h>

void delete_message_and_warn(dpp::cluster& bot, const dpp::message_create_t ev, const dpp::attachment attach, const std::string text)
{
	bot.message_delete(ev.msg.id, ev.msg.channel_id, [&bot, ev, attach, text](const auto& cc) {

		if (cc.is_error()) {
			bad_embed("Error", bot, ev.msg.channel_id, "Failed to delete the message: " + ev.msg.id.str(), ev.msg);
			return;
		}

		db::resultset logchannel = db::query("SELECT log_channel, embed_title, embed_body FROM guild_config WHERE guild_id = '?'", { ev.msg.guild_id.str() });
		if (logchannel.size()) {
			std::string message_body = logchannel[0].at("embed_body");
			std::string message_title = logchannel[0].at("embed_title");
			if (message_body.empty()) {
				message_body = "Please configure a message!";
			}
			if (message_title.empty()) {
				message_title = "Yeet!";
			}
			message_body = replace_string(message_body, "@user", "<@" + ev.msg.author.id.str() + ">");
			bad_embed(message_title, bot, ev.msg.channel_id, message_body, ev.msg);
			good_embed(
				bot, dpp::snowflake(logchannel[0].at("log_channel")), "Attachment: `" + attach.filename + "`\nSent by: `" +
				ev.msg.author.format_username() + "`\nMatched pattern: `" + text + "`\n[Image link](" + attach.url +")"
			);
		}
	});
}
