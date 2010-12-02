///////////////////////////////////////////////////////////////////////////////
//                                                                             
//  Copyright (C) 2010  Artyom Beilis (Tonkikh) <artyomtnk@yahoo.com>     
//                                                                             
//  This program is free software: you can redistribute it and/or modify       
//  it under the terms of the GNU Lesser General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
///////////////////////////////////////////////////////////////////////////////
#define CPPDB_SOURCE
#include <cppdb/driver_manager.h>
#include <cppdb/shared_object.h>
#include <cppdb/backend.h>
#include <cppdb/utils.h>
#include <cppdb/mutex.h>

#include <vector>
#include <list>

extern "C" {
	#ifdef CPPDB_WITH_SQLITE3 
	CPPDB_API cppdb::backend::connection *cppdb_sqlite3_get_connection(cppdb::connection_info const &cs);
	#endif
	#ifdef CPPDB_WITH_PQ 
	CPPDB_API cppdb::backend::connection *cppdb_postgresql_get_connection(cppdb::connection_info const &cs);
	#endif
	#ifdef CPPDB_WITH_ODBC
	CPPDB_API cppdb::backend::connection *cppdb_odbc_get_connection(cppdb::connection_info const &cs);
	#endif
	#ifdef CPPDB_WITH_MYSQL
	CPPDB_API cppdb::backend::connection *cppdb_mysql_get_connection(cppdb::connection_info const &cs);
	#endif
}


namespace cppdb {

	typedef backend::static_driver::connect_function_type connect_function_type;

	class so_driver : public backend::loadable_driver {
	public:
		so_driver(std::string const &name,std::vector<std::string> const &so_list) :
			connect_(0)
		{
			std::string symbol_name = "cppdb_" + name + "_get_connection";
			for(unsigned i=0;i<so_list.size();i++) {
				so_ = shared_object::open(so_list[i]);
				if(so_) {
					so_->safe_resolve(symbol_name,connect_);
					break;
				}
			}
			if(!so_ || !connect_) {
				throw cppdb_error("cppdb::driver failed to load driver " + name + " - no module found");
			}
		}
		virtual bool in_use()
		{
			return use_count() == 1;
		}
		virtual backend::connection *open(connection_info const &ci)
		{
			return connect_(ci);
		}
	private:
		connect_function_type connect_;
		ref_ptr<shared_object> so_;
	};
	
	backend::connection *driver_manager::connect(std::string const &str)
	{
		connection_info conn(str);
		return connect(conn);
	}

	backend::connection *driver_manager::connect(connection_info const &conn)
	{
		ref_ptr<backend::driver> drv_ptr;
		drivers_type::iterator p;
		{ // get driver
			mutex::guard lock(lock_);
			p=drivers_.find(conn.driver);
			if(p!=drivers_.end()) {
				drv_ptr = p->second;
			}
			else {
				drv_ptr = load_driver(conn);
				drivers_[conn.driver] = drv_ptr;	
			}
		}
		return drv_ptr->connect(conn);
	}
	void driver_manager::collect_unused()
	{
		std::list<ref_ptr<backend::driver> > garbage;
		{
			mutex::guard lock(lock_);
			drivers_type::iterator p=drivers_.begin(),tmp;
			while(p!=drivers_.end()) {
				if(!p->second->in_use()) {
					garbage.push_back(p->second);
					tmp=p;
					++p;
					drivers_.erase(tmp);
				}
				else {
					++p;
				}
			}
		}
		garbage.clear();
	}

	// TODO Fix Me
	#define CPPDB_LIBRARY_SO_VERSION "0"
	// TODO Fix Me

	#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) || defined(__CYGWIN__)
	
	#	define CPPDB_LIBRARY_SUFFIX_V1 CPPDB_LIBRARY_SUFFIX
	#	define CPPDB_LIBRARY_SUFFIX_V2 "-" CPPDB_LIBRARY_SO_VERSION CPPDB_LIBRARY_SUFFIX

	#else

	#	define CPPDB_LIBRARY_SUFFIX_V1 CPPDB_LIBRARY_SUFFIX
	#	define CPPDB_LIBRARY_SUFFIX_V2 CPPDB_LIBRARY_SUFFIX "." CPPDB_LIBRARY_SO_VERSION

	#endif

	ref_ptr<backend::driver> driver_manager::load_driver(connection_info const &conn)
	{
		std::vector<std::string> so_names;
		std::string module;
		if(!(module=conn.get("cppdb_module")).empty()) {
			so_names.push_back(module);
		}
		else {
			std::string so_name1 = CPPDB_LIBRARY_PREFIX "cppdb_" + conn.driver + CPPDB_LIBRARY_SUFFIX_V1;
			std::string so_name2 = CPPDB_LIBRARY_PREFIX "cppdb_" + conn.driver + CPPDB_LIBRARY_SUFFIX_V2;

			for(unsigned i=0;i<search_paths_.size();i++) {
				so_names.push_back(search_paths_[i]+"/" + so_name1);
				so_names.push_back(search_paths_[i]+"/" + so_name2);
			}
			if(!no_default_directory_) {
				so_names.push_back(so_name1);
				so_names.push_back(so_name2);
			}
		}
		ref_ptr<backend::driver> drv=new so_driver(conn.driver,so_names);
		return drv;
	}

	void driver_manager::install_driver(std::string const &name,ref_ptr<backend::driver> drv)
	{
		if(!drv) {
			throw cppdb_error("cppdb::driver_manager::install_driver: Can't install empty driver");
		}
		mutex::guard lock(lock_);
		drivers_[name]=drv;
	}

	driver_manager::driver_manager() : 
		no_default_directory_(false)
	{
	}
	driver_manager::~driver_manager()
	{
	}
	
	void driver_manager::add_search_path(std::string const &p)
	{
		mutex::guard l(lock_);
		search_paths_.push_back(p);
	}
	void driver_manager::clear_search_paths()
	{
		mutex::guard l(lock_);
		search_paths_.clear();
	}
	void driver_manager::use_default_search_path(bool v)
	{
		mutex::guard l(lock_);
		no_default_directory_ = !v;
	}
	driver_manager &driver_manager::instance()
	{
		static driver_manager instance;
		return instance;
	}
	namespace {
		struct initializer {
			initializer() { 
				driver_manager::instance(); 
				#ifdef CPPDB_WITH_SQLITE3 
				driver_manager::instance().install_driver(
					"sqlite3",new backend::static_driver(cppdb_sqlite3_get_connection)
				);
				#endif
				#ifdef CPPDB_WITH_ODBC
				driver_manager::instance().install_driver(
					"odbc",new backend::static_driver(cppdb_odbc_get_connection)
				);
				#endif
				#ifdef CPPDB_WITH_PQ 
				driver_manager::instance().install_driver(
					"postgresql",new backend::static_driver(cppdb_postgresql_get_connection)
				);
				#endif
				#ifdef CPPDB_WITH_MYSQL
				driver_manager::instance().install_driver(
					"mysql",new backend::static_driver(cppdb_mysql_get_connection)
				);
				#endif
			}
			
		} init;
	}
} // cppdb
