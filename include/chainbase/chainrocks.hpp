// TODO: Add namespace
// TODO: Add respective `rocksdb` includes

/**
 *  An "undo-session" is a period of time in which a sequence of transactions gets evaluated, and if there
 *  are any conficts between differing transactions, those transactions will get "popped" off the undo deque
 *  until the satisfiying conditions are met.
 */

#pragma once

#include <deque>   // std::deque
#include <map>     // std::map
#include <set>     // std::set, std::multiset
#include <sstream> // std::stringstream
#include <memory>  // std::unique_ptr
#include <vector>  // std::vector

#include <boost/filesystem.hpp> // bfs::path

#include <rocksdb/db.h> // rocksdb::DB
                        // rocksdb::Options, rocksdb::ReadOptions, rocksdb::WriteOptions
                        // rocksdb::Slice
                        // rocksdb::Status

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

namespace bfs = boost::filesystem;

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 *  This data structure is responsible for the context of the undo-session.
 *  It answers the questions, "What type of data will I be working with in this undo-session"?
 */
template<typename value_type>
class undo_state
{
public:
   template<typename T>
   undo_state()
   : _old_values{}
   , _removed_values{}
   , _new_indexes{}
   , _old_next_index{}
   , _revision{}
   {
   }

   std::map<uint64_t, value_type, std::less<uint64_t>>; _old_values;
   std::map<uint64_t, value_type, std::less<uint64_t>>; _removed_values;
   std::set<uint64_t,             std::less<uint64_t>>; _new_indexes;
   uint64_t                                             _old_next_index;
   int64_t                                              _revision;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 *  This data structure is responsible for managing
 */
class index
{
public:
   /**
    *  Stuff
    */
   index()
   : _stack{}
   , _indices{}
   {
   }

   /**
    *  Stuff
    */
   template<typename Constructor>
   const std::vector<uint8_t>& emplace(Constructor&& c) {
      uint64_t new_id{_next_id};

      auto constructor{[&](std::vector<uint8_t>& value) { value.id = new_id; c{value}; }};

      auto insert_result{_indices.emplace(constructor)};

      if (!insert_result.second) {
         BOOST_THROW_EXCEPTION(std::logic_error("could not insert object, most likely a uniqueness constraint was violated"));
      }

      ++_next_id;
      on_create(*insert_result.first);
            
      return *insert_result.first;
   }

   /**
    *  Stuff
    */
   template<typename Modifier>
   void modify(const std::vector<uint8_t>& obj, Modifier&& m) {
      on_modify(obj);
            
      auto ok{_indices.modify(_indices.iterator_to(obj), m)};
            
      if(!ok) {
         std::abort(); // Uniqueness violation.
      }
   }

   /**
    *  Stuff
    */
   void remove(const std::vector<uint8_t>& obj) {
      on_remove(obj);
            
      _indices.erase(_indices.iterator_to(obj));
   }

   /**
    *  Stuff
    */
   const std::vector<uint8_t>* find(uint64_t&& key) const {
      auto iter{_indices.find(std::forward<uint64_t>(key))};
                
      if(iter != _indices.end()) {
         return &*itr;
      }   
      return nullptr;
   }

   const std::vector<uint8_t>& get(uint64_t&& key) const {
      auto ptr{find(key)};
      if(!ptr) {
         std::stringstream ss;
         ss << "key not found (" << boost::core::demangle(typeid(key).name()) << "): " << key;
         BOOST_THROW_EXCEPTION(std::out_of_range(ss.str().c_str()));
      }
      return *ptr;
   }

   const uint64_t& indices() const {
      return _indices;
   }

   class session {
   public:
      session(session&& mv)
         : _index{mv._index}
         , _apply{mv._apply}
      {
         mv._apply = false;
      }

      ~session() {
         if(_apply) {
            _index.undo();
         }
      }

      /** leaves the UNDO state on the stack when session goes out of scope */
      void push() {
         _apply = false;
      }
            
      /** combines this session with the prior session */
      void squash() {
         if (_apply) {
            _index.squash();
         }
         _apply = false;
      }
            
      void undo() {
         if (_apply) {
            _index.undo();
         }
         _apply = false;
      }

      session& operator = (session&& mv) {
         if(this == &mv) {
            return *this;
         }
                
         if(_apply) {
            _index.undo();
         }
                
         _apply    = mv._apply;
         mv._apply = false;
                
         return *this;
      }

      int64_t revision() const {
         return _revision;
      }

   private:
      friend class index;

      session(index& idx, int64_t revision)
         : _index(idx)
         , _revision(revision)
      {
         if(revision == -1) {
            _apply = false;
         }
      }

      index&  _index;
      bool    _apply{true};
      int64_t _revision{};
   };

   session start_undo_session(bool enabled) {
      if(enabled) {
         _stack.emplace_back(_indices.get_allocator());
             
         _stack.back().old_next_id = _next_id;
         _stack.back().revision    = ++_revision;
             
         return session{*this, _revision};
      }
      else {
         return session{*this, -1};
      }
   }

   int64_t revision() const {
      return _revision;
   }

   /**
    *  Stuff
    */
   void undo() {
      if(!stuff_to_undo()) {
         return;
      }

      const auto& head{_stack.back()};

      for(auto id : head.new_ids) {
         _indices.erase( _indices.find(id));
      }
      _next_id = head.old_next_id;

      for(auto& item : head.old_values) {
         auto ok{_indices.modify(_indices.find(item.second.id), [&](std::vector<uint8_t>& v) { v = std::move(item.second); })};
         if(!ok) {
            std::abort(); // uniqueness violation
         }
      }

      for(auto& item : head.removed_values) {
         bool ok{_indices.emplace(std::move(item.second)).second};
         if(!ok) {
            std::abort(); // uniqueness violation
         }
      }

      _stack.pop_back();
      --_revision;
   }

   /**
    *  Stuff
    */
   void squash() {
      if(!stuff_to_undo()) {
         return;
      }
            
      if(_stack.size() == 1) {
         _stack.pop_front();
         --_revision;
         return;
      }

      auto& state{_stack.back()};
      auto& prev_state{_stack[_stack.size()-2]};

      /**
       *  Stuff
       */
      for(const auto& item : state.old_values){
         if (prev_state.new_ids.find(item.second.id) != prev_state.new_ids.end()) {
            // new+upd -> new, type A
            continue;
         }
               
         if(prev_state.old_values.find(item.second.id) != prev_state.old_values.end()) {
            // upd(was=X) + upd(was=Y) -> upd(was=X), type A
            continue;
         }
         // del+upd -> N/A
         assert(prev_state.removed_values.find(item.second.id) == prev_state.removed_values.end());
               
         // nop+upd(was=Y) -> upd(was=Y), type B
         prev_state.old_values.emplace(std::move(item));
      }

      // *+new, but we assume the N/A cases don't happen, leaving type B nop+new -> new
      for(auto id : state.new_ids) {
         prev_state.new_ids.insert(id);
      }   

      // *+del
      for(auto& obj : state.removed_values) {
         if(prev_state.new_ids.find(obj.second.id) != prev_state.new_ids.end()) {
            // new + del -> nop (type C)
            prev_state.new_ids.erase(obj.second.id);
            continue;
         }
               
         auto iter{prev_state.old_values.find(obj.second.id)};
               
         if(iter != prev_state.old_values.end()) {
            // upd(was=X) + del(was=Y) -> del(was=X)
            prev_state.removed_values.emplace( std::move(*iter) );
            prev_state.old_values.erase(obj.second.id);
            continue;
         }
               
         // del + del -> N/A
         assert(prev_state.removed_values.find(obj.second.id) == prev_state.removed_values.end());
               
         // nop + del(was=Y) -> del(was=Y)
         prev_state.removed_values.emplace(std::move(obj)); //[obj.second->id] = std::move(obj.second);
      }
            
      _stack.pop_back();
      --_revision;
   }

   /**
    * Stuff
    */
   void commit(int64_t revision)
   {
      while(_stack.size() && _stack[0].revision <= revision) {
         _stack.pop_front();
      }
   }

   /**
    * Stuff
    */
   void undo_all() {
      while(stuff_to_undo()) {
         undo();
      }
   }

   /**
    * Stuff
    */
   void set_revision(uint64_t revision) {
      if(_stack.size() != 0) {
         BOOST_THROW_EXCEPTION(std::logic_error("cannot set revision while there is an existing undo stack"));
      }
            
      if(revision > std::numeric_limits<int64_t>::max()) {
         BOOST_THROW_EXCEPTION(std::logic_error("revision to set is too high"));
      }
            
      _revision = static_cast<int64_t>(revision);
   }

   /**
    * Stuff
    */
   void remove_object(int64_t id) {
      const std::vector<uint8_t>* val{std::find(typename std::vector<uint8_t>::id_type(id))};
      if(!val) {
         BOOST_THROW_EXCEPTION(std::out_of_range(std::to_string(id)));
      }
      remove(*val);
   }

   /**
    * Stuff
    */
   std::pair<int64_t, int64_t> undo_stack_revision_range() const {
      int64_t begin{_revision};
      int64_t end{_revision};

      if(_stack.size() > 0) {
         begin = _stack.front().revision - 1;
         end   = _stack.back().revision;
      }

      return {begin, end};
   }

   /**
    * Stuff
    */
   const auto& stack() const {
      return _stack;
   }

private:
   /**
    * Stuff
    */
   bool stuff_to_undo() const {
      return _stack.size();
   }

   /**
    * Stuff
    */
   void on_modify(const std::vector<uint8_t>& value) {
      if (!stuff_to_undo()) {
         return;
      }

      auto& head{_stack.back()};

      if(head.new_ids.find(value.id) != head.new_ids.end()) {
         return;
      }
            
      auto iter{head.old_values.find(value.id)};
      if(iter != head.old_values.end()) {
         return;
      }
            
      head.old_values.emplace(std::pair<typename std::vector<uint8_t>::id_type, const std::vector<uint8_t>&>(value.id, value)); // RocksDB insert
   }

   /**
    * Stuff
    */
   void on_remove(const std::vector<uint8_t>& value) {
      if(!stuff_to_undo()) {
         return;
      }

      auto& head{_stack.back()};
      if(head.new_ids.count(value.id)) {
         head.new_ids.erase(value.id);
         return;
      }

      auto iter{head.old_values.find(value.id)};
      if (iter != head.old_values.end()) {
         head.removed_values.emplace(std::move(*iter));
         head.old_values.erase(value.id);
         return;
      }

      if(head.removed_values.count(value.id)) {
         return;
      }   

      // Don't need id_type cuz RocksDB
      head.removed_values.emplace(std::pair<typename std::vector<uint8_t>::id_type, const std::vector<uint8_t>&>(value.id, value));
   }

   /**
    * Stuff
    */
   void on_create(const std::vector<uint8_t>& value) {
      if(!stuff_to_undo()) {
         return;
      }
      auto& head{_stack.back()};
      head.new_ids.insert(value.id);
   }

   /**
    * Stuff
    */
   std::deque<undo_state<std::vector<uint8_t>>> _stack{};
   uint64_t                                     _indices{};
   uint64_t                                     _next_id{};
   int64_t                                      _revision{};
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

class abstract_session {
public:
   virtual ~abstract_session(){};
   virtual void push()              = 0;
   virtual void squash()            = 0;
   virtual void undo()              = 0;
   virtual int64_t revision( )const = 0;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

class rocksdb_options {
public:
   rocksdb_options() {
      _general_options.create_if_missing = true;
      _general_options.IncreaseParallelism();
      _general_options.OptimizeLevelStyleCompaction();
   }
   
   const rocksdb::Options& general_options() {
      return _general_options;
   }
   
   const rocksdb::ReadOptions& read_options() {
      return _read_options;
   }
         
   const rocksdb::WriteOptions& write_options() {
      return _write_options;
   }
         
private:
   rocksdb::Options      _general_options;
   rocksdb::ReadOptions  _read_options;
   rocksdb::WriteOptions _write_options;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
   
class key {
public:
   key(uint64_t key)
      : _key{key}
   {
   }
      
   using key_type = uint64_t;
      
   explicit operator rocksdb::Slice() const {
      return rocksdb::Slice{std::to_string(_key)};
   }
      
private:
   key_type _key;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

class value {
public:
   value(const std::vector<uint8_t>& value)
      : _value{std::vector<char>(value.cbegin(), value.cend())}
   {
   }
      
   using value_type = std::vector<char>;
      
   explicit operator rocksdb::Slice() const {
      return rocksdb::Slice{_value.data(), _value.size()};
   }
      
private:
   value_type _value;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
   
class rocksdb_database {
public:
   enum class database_mode : uint8_t {
      read_only  = 1,
      read_write = 2
   };
         
   rocksdb_database(const bfs::path& directory, database_mode mode = database_mode::read_write)
   {
      if (mode == database_mode::read_write) {
         _status = rocksdb::DB::Open(_options.general_options(), directory.string(), &_database);
         _is_read_only = false;
         _check_status();
      }
      else if (mode == database_mode::read_only) {
         _status = rocksdb::DB::OpenForReadOnly(_options.general_options(), directory.string(), &_database);
         _is_read_only = true;
         _check_status();
      }
   }

   rocksdb_database(const rocksdb_database&) = delete;
   rocksdb_database& operator = (const rocksdb_database&) = default;
      
   rocksdb_database(rocksdb_database&&) = delete;
   rocksdb_database& operator = (rocksdb_database&&) = default;
   
   ~rocksdb_database() {
      _database->Close();
      _check_status();
      delete _database;
   }

   bool is_read_only() const {
      return _is_read_only;
   }

   struct session {
   public:
      session(session&& sesh)
         : _index_sessions{std::move(sesh._index_sessions)},
         , _revision(sesh._revision)
      {
      }
         
      session(std::vector<std::unique_ptr<abstract_session>>&& sesh)
         : _index_sessions(std::move(sesh))
      {
         if (_index_sessions.size())
            _revision = _index_sessions[0]->revision();
      }

      ~session() {
         undo();
      }

      void push()
      {
         for(auto& sesh : _index_sessions) {
            sesh->push();
         }
         _index_sessions.clear();
      }

      void squash()
      {
         for(auto& sesh : _index_sessions) {
            sesh->squash();
         }
         _index_sessions.clear();
      }

      void undo()
      {
         for(auto& sesh : _index_sessions) {
            sesh->undo();
         }
         _index_sessions.clear();
      }

      int64_t revision() const {
         return _revision;
      }

   private:
      friend class rocksdb_database;

      std::vector<std::unique_ptr<abstract_session>> _index_sessions;
      int64_t                                        _revision{-1};
   };

   session start_undo_session(bool enabled) {
      if (enabled) {
         vector<std::unique_ptr<abstract_session>> _sub_sessions;
         _sub_sessions.reserve(_index_list.size());
         for(auto& item : _index_list) {
            _sub_sessions.push_back(item->start_undo_session(enabled));
         }
         return session{std::move(_sub_sessions)};
      } else {
         return session{};
      }
   }

   int64_t revision() const {
      if (_index_list.size() == 0) {
         return -1;
      }
      return _index_list[0]->revision();
   }

   void set_revision(uint64_t revision) {
      for(auto i : _index_list) {
         i->set_revision(revision);
      }
   }

   void undo() {
      for(auto& item : _index_list){
         item->undo();
      }
   }
      
   void squash() {
      for(auto& item : _index_list) {
         item->squash();
      }
   }
      
   void commit(int64_t revision) {
      for(auto& item : _index_list) {
         item->commit(revision);
      }
   }
      
   void undo_all() {
      for(auto& item : _index_list) {
         item->undo_all();
      }
   }

   void add_index(const class key& key, const value& value) {
      if (!_is_read_only) {
         put(key, value);
      }
      else {
         BOOST_THROW_EXCEPTION(std::runtime_error("Cannot add index to `read_only` database"));
      }
         
      // Ensure the undo stack of added index is consistent with the other indices in the database
      if( _index_list.size() > 0 ) {
         auto expected_revision_range = _index_list.front()->undo_stack_revision_range();
         auto added_index_revision_range = idx_ptr->undo_stack_revision_range();

         if( added_index_revision_range.first != expected_revision_range.first ||
             added_index_revision_range.second != expected_revision_range.second ) {

            if( !first_time_adding ) {
               BOOST_THROW_EXCEPTION( std::logic_error(
                                         "existing index for " + type_name + " has an undo stack (revision range [" +
                                         std::to_string(added_index_revision_range.first) + ", " +
                                         std::to_string(added_index_revision_range.second) +
                                         "]) that is inconsistent with other indices in the database (revision range [" +
                                         std::to_string(expected_revision_range.first) + ", " +
                                         std::to_string(expected_revision_range.second) +
                                         "]); corrupted database?"));
            }

            idx_ptr->set_revision(static_cast<uint64_t>(expected_revision_range.first));
            while(idx_ptr->revision() < expected_revision_range.second) {
               idx_ptr->start_undo_session(true).push();
            }
         }
      }

      if(type_id >= _index_map.size())
         _index_map.resize(type_id + 1);

      auto new_index = new index<index_type>( *idx_ptr );
      _index_map[ type_id ].reset( new_index );
      _index_list.push_back( new_index );
   }
   
   void get(const class key& key, std::string &value) {
      _status = _database->Get(_options.read_options(), slice_it_up(key), &value);
      _check_status();
   }
   
   void put(const class key& key, const value& value) {
      _status = _database->Put(_options.write_options(), slice_it_up(key), slice_it_up(value));
      _check_status();
   }
   
   void remove(const class key& key) {
      _status = _database->Delete(_options.write_options(), slice_it_up(key));
      _check_status();
   }
   
   // Deteremined by user-defined merge operator
   void merge(const class key& key, const value& value) {
      _status = _database->Merge(_options.write_options(), slice_it_up(key), slice_it_up(value));
      _check_status();
   }
         
private:
   rocksdb::DB*                            _database;
   rocksdb::Status                         _status;
   bfs::path                               _data_dir;
   std::vector<abstract_index*>            _index_list;
   std::vector<unique_ptr<abstract_index>> _index_map;
   rocksdb_options                         _options;
   bool                                    _is_read_only;
   
   inline void _check_status() {
      if (_status.ok()) { // TODO: Use the individual status numbers
         return;
      }
      else {
         std::cerr << _status.ToString() << std::endl;
         BOOST_THROW_EXCEPTION(std::runtime_error("Unhandled `rocksdb` status"));
      }
   }
   
   template<typename T>
   inline const rocksdb::Slice slice_it_up(const T& t) {
      return static_cast<const rocksdb::Slice>(t);
   }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
