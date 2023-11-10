/************************************************************************************
 * 
 * Sporks, the learning, scriptable Discord bot!
 *
 * Copyright 2019 Craig Edwards <support@sporks.gg>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/

#include <beholder/beholder.h>
#include <beholder/database.h>
#include <beholder/config.h>
#include <beholder/sentry.h>
#include <mysql/mysql.h>
#include <fmt/format.h>
#include <iostream>
#include <mutex>
#include <sstream>

#ifdef MARIADB_VERSION_ID
	#define CONNECT_STRING "SET @@SESSION.max_statement_time=3000"
#else
	#define CONNECT_STRING "SET @@SESSION.max_execution_time=3000"
#endif

namespace db {

	MYSQL connection;
	std::mutex db_mutex;
	std::string _error;
	dpp::cluster* creator = nullptr;

	/**
	 * Connect to mysql database, returns false if there was an error.
	 */
	bool connect(const std::string &host, const std::string &user, const std::string &pass, const std::string &db, int port) {
		std::lock_guard<std::mutex> db_lock(db_mutex);
		if (mysql_init(&connection) != nullptr) {
			mysql_options(&connection, MYSQL_INIT_COMMAND, CONNECT_STRING);
			return mysql_real_connect(&connection, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), port, NULL, CLIENT_MULTI_RESULTS | CLIENT_MULTI_STATEMENTS);
		} else {
			_error = "mysql_init() failed";
			return false;
		}
	}

	void init (dpp::cluster& bot)
	{
		creator = &bot;
		const json& dbconf = config::get("database");
		if (!db::connect(dbconf["host"], dbconf["username"], dbconf["password"], dbconf["database"], dbconf["port"])) {
			creator->log(dpp::ll_critical, fmt::format("Database connection error connecting to {}", dbconf["database"]));
			exit(2);
		}
		creator->log(dpp::ll_info, fmt::format("Connected to database: {}", dbconf["database"]));
	}

	/**
	 * Disconnect from mysql database, for now always returns true.
	 * If there's an error, there isn't much we can do about it anyway.
	 */
	bool close() {
		std::lock_guard<std::mutex> db_lock(db_mutex);
		mysql_close(&connection);
		return true;
	}

	const std::string& error() {
		return _error;
	}

	/**
	 * Run a mysql query, with automatic escaping of parameters to prevent SQL injection.
	 * The parameters given should be a vector of strings. You can instantiate this using "{}".
	 * For example: db::query("UPDATE foo SET bar = '?' WHERE id = '?'", {"baz", "3"});
	 * Returns a resultset of the results as rows. Avoid returning massive resultsets if you can.
	 */
	resultset query(const std::string &format, const paramlist &parameters) {

		/**
		 * One DB handle can't query the database from multiple threads at the same time.
		 * To prevent corruption of results, put a lock guard on queries.
		 */
		std::lock_guard<std::mutex> db_lock(db_mutex);
		std::vector<std::string> escaped_parameters;
		std::vector<std::string> unescaped_parameters;
		resultset rv;

		_error.clear();

		/**
		 * Escape all parameters properly from a vector of std::variant
		 */
		for (const auto& param : parameters) {
			/* Worst case scenario: Every character becomes two, plus NULL terminator*/
			std::visit([parameters, &escaped_parameters, &unescaped_parameters](const auto &p) {
				std::ostringstream v;
				v << p;
				std::string s_param(v.str());
				char out[s_param.length() * 2 + 1];
				/* Some moron thought it was a great idea for mysql_real_escape_string to return an unsigned but use -1 to indicate error.
				 * This stupid cast below is the actual recommended error check from the reference manual. Seriously stupid.
				 */
				unescaped_parameters.push_back(out);
				if (mysql_real_escape_string(&connection, out, s_param.c_str(), s_param.length()) != (unsigned long)-1) {
					escaped_parameters.push_back(out);
				}
			}, param);
		}

		if (parameters.size() != escaped_parameters.size()) {
			_error = "Parameter wasn't escaped; error: " + std::string(mysql_error(&connection));
			creator->log(dpp::ll_error, _error);
			return rv;
		}

		unsigned int param = 0;
		std::string querystring;

		/**
		 * Search and replace escaped parameters in the query string.
		 *
		 * TODO: Really, I should use a cached query and the built in parameterisation for this.
		 *       It would scale a lot better.
		 */
		for (auto v = format.begin(); v != format.end(); ++v) {
			if (*v == '?' && escaped_parameters.size() >= param + 1) {
				querystring.append(escaped_parameters[param]);
				if (param != escaped_parameters.size() - 1) {
					param++;
				}
			} else {
				querystring += *v;
			}
		}

		void *qlog = sentry::start_transaction(sentry::register_transaction_type("PID#" + std::to_string(getpid()), "db"));
		void* qspan = sentry::span(qlog, querystring);

		int result = mysql_query(&connection, querystring.c_str());

		/**
		 * On successful query collate results into a std::map
		 */
		if (result == 0) {
			MYSQL_RES *a_res = mysql_use_result(&connection);
			if (a_res) {
				MYSQL_ROW a_row;
				while ((a_row = mysql_fetch_row(a_res))) {
					MYSQL_FIELD *fields = mysql_fetch_fields(a_res);
					row thisrow;
					unsigned int field_count = 0;
					if (mysql_num_fields(a_res) == 0) {
						break;
					}
					if (fields && mysql_num_fields(a_res)) {
						while (field_count < mysql_num_fields(a_res)) {
							std::string a = (fields[field_count].name ? fields[field_count].name : "");
							std::string b = (a_row[field_count] ? a_row[field_count] : "");
							thisrow[a] = b;
							field_count++;
						}
						rv.push_back(thisrow);
					}
				}
				mysql_free_result(a_res);
			}
			sentry::set_span_status(qspan, sentry::STATUS_OK);
		} else {
			/**
			 * In properly written code, this should never happen. Famous last words.
			 */
			_error = mysql_error(&connection);
			sentry::set_span_status(qspan, sentry::STATUS_INVALID_ARGUMENT);
			creator->log(dpp::ll_error, fmt::format("{} (query: {})", _error, querystring));
		}

		sentry::end_span(qspan);
		sentry::end_transaction(qlog);

		return rv;
	}
};
