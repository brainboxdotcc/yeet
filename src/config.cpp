#include <dpp/dpp.h>
#include <dpp/json.h>
#include <fstream>
#include <beholder/beholder.h>

namespace config {

	static json configdocument;

	void init(const std::string& config_file) {
		/* Set up the bot cluster and read the configuration json */
		std::ifstream configfile(config_file);
		configfile >> configdocument;
	}

	json& get(const std::string& key) {
		if (key.empty()) {
			return configdocument;
		}
		return configdocument.at(key);
	}
};