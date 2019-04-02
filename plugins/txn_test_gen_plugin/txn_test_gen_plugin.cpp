/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#include <eosio/txn_test_gen_plugin/txn_test_gen_plugin.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/wast_to_wasm.hpp>

#include <fc/variant.hpp>
#include <fc/io/json.hpp>
#include <fc/exception/exception.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/io/json.hpp>

#include <boost/asio/high_resolution_timer.hpp>
#include <boost/algorithm/clamp.hpp>

#include <Inline/BasicTypes.h>
#include <IR/Module.h>
#include <IR/Validate.h>
#include <WAST/WAST.h>
#include <WASM/WASM.h>
#include <Runtime/Runtime.h>

#include <contracts.hpp>

using namespace eosio::testing;

namespace eosio { namespace detail {
  struct txn_test_gen_empty {};
}}

FC_REFLECT(eosio::detail::txn_test_gen_empty, );

namespace eosio {

static appbase::abstract_plugin& _txn_test_gen_plugin = app().register_plugin<txn_test_gen_plugin>();

using namespace eosio::chain;
using io_work_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

#define CALL(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   [this](string, string body, url_response_callback cb) mutable { \
          try { \
             if (body.empty()) body = "{}"; \
             INVOKE \
             cb(http_response_code, fc::json::to_string(result)); \
          } catch (...) { \
             http_plugin::handle_exception(#api_name, #call_name, body, cb); \
          } \
       }}

#define INVOKE_V_R_R_R(api_handle, call_name, in_param0, in_param1, in_param2) \
     const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
     api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>(), vs.at(2).as<in_param2>()); \
     eosio::detail::txn_test_gen_empty result;

#define INVOKE_V_R_R(api_handle, call_name, in_param0, in_param1) \
     const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
     api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>()); \
     eosio::detail::txn_test_gen_empty result;

#define INVOKE_V_V(api_handle, call_name) \
     api_handle->call_name(); \
     eosio::detail::txn_test_gen_empty result;

#define CALL_ASYNC(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   [this](string, string body, url_response_callback cb) mutable { \
      if (body.empty()) body = "{}"; \
      auto result_handler = [cb, body](const fc::exception_ptr& e) {\
         if (e) {\
            try {\
               e->dynamic_rethrow_exception();\
            } catch (...) {\
               http_plugin::handle_exception(#api_name, #call_name, body, cb);\
            }\
         } else {\
            cb(http_response_code, fc::json::to_string(eosio::detail::txn_test_gen_empty())); \
         }\
      };\
      INVOKE \
   }\
}

#define INVOKE_ASYNC_R_R(api_handle, call_name, in_param0, in_param1) \
   const auto& vs = fc::json::json::from_string(body).as<fc::variants>(); \
   api_handle->call_name(vs.at(0).as<in_param0>(), vs.at(1).as<in_param1>(), result_handler);

struct txn_test_gen_plugin_impl {

   uint64_t _total_us = 0;
   uint64_t _txcount = 0;
   time_point _start_time;

   std::shared_ptr<boost::asio::io_context>             gen_ioc;
   optional<io_work_t>                                  gen_ioc_work;
   uint16_t                                             thread_pool_size;
   optional<boost::asio::thread_pool>                   thread_pool;
   std::shared_ptr<boost::asio::high_resolution_timer>  timer;

   void push_next_transaction(const std::shared_ptr<std::vector<signed_transaction>>& trxs, const std::function<void(const fc::exception_ptr&)>& next ) {
      chain_plugin& cp = app().get_plugin<chain_plugin>();

      for (size_t i = 0; i < trxs->size(); ++i) {
         cp.accept_transaction( packed_transaction(trxs->at(i)), [=](const fc::static_variant<fc::exception_ptr, transaction_trace_ptr>& result){
            if (result.contains<fc::exception_ptr>()) {
               next(result.get<fc::exception_ptr>());
            } else {
               if (result.contains<transaction_trace_ptr>() && result.get<transaction_trace_ptr>()->receipt) {
                  _total_us += result.get<transaction_trace_ptr>()->receipt->cpu_usage_us;
                  ++_txcount;
               }
            }
         });
      }
   }

   void push_transactions( std::vector<signed_transaction>&& trxs, const std::function<void(fc::exception_ptr)>& next ) {
      auto trxs_copy = std::make_shared<std::decay_t<decltype(trxs)>>(std::move(trxs));
      app().post(priority::low, [this, trxs_copy, next]() {
         push_next_transaction(trxs_copy, next);
      });
   }

   void create_test_accounts(const std::string& init_name, const std::string& init_priv_key, const std::function<void(const fc::exception_ptr&)>& next) {
      std::vector<signed_transaction> trxs;
      trxs.reserve(2);

      try {
         name newaccountA("txn.test.a");
         name newaccountB("txn.test.b");
         name newaccountC("txn.test.t");
         name creator(init_name);

         abi_def currency_abi_def = fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>();

         controller& cc = app().get_plugin<chain_plugin>().chain();
         auto chainid = app().get_plugin<chain_plugin>().get_chain_id();
         auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();

         abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(), abi_serializer_max_time};

         fc::crypto::private_key txn_test_receiver_A_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'a')));
         fc::crypto::private_key txn_test_receiver_B_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'b')));
         fc::crypto::private_key txn_test_receiver_C_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'c')));
         fc::crypto::public_key  txn_text_receiver_A_pub_key = txn_test_receiver_A_priv_key.get_public_key();
         fc::crypto::public_key  txn_text_receiver_B_pub_key = txn_test_receiver_B_priv_key.get_public_key();
         fc::crypto::public_key  txn_text_receiver_C_pub_key = txn_test_receiver_C_priv_key.get_public_key();
         fc::crypto::private_key creator_priv_key = fc::crypto::private_key(init_priv_key);

         //create some test accounts
         {
            signed_transaction trx;

            //create "A" account
            {
            auto owner_auth   = eosio::chain::authority{1, {{txn_text_receiver_A_pub_key, 1}}, {}};
            auto active_auth  = eosio::chain::authority{1, {{txn_text_receiver_A_pub_key, 1}}, {}};

            trx.actions.emplace_back(vector<chain::permission_level>{{creator,"active"}}, newaccount{creator, newaccountA, owner_auth, active_auth});
            }
            //create "B" account
            {
            auto owner_auth   = eosio::chain::authority{1, {{txn_text_receiver_B_pub_key, 1}}, {}};
            auto active_auth  = eosio::chain::authority{1, {{txn_text_receiver_B_pub_key, 1}}, {}};

            trx.actions.emplace_back(vector<chain::permission_level>{{creator,"active"}}, newaccount{creator, newaccountB, owner_auth, active_auth});
            }
            //create "txn.test.t" account
            {
            auto owner_auth   = eosio::chain::authority{1, {{txn_text_receiver_C_pub_key, 1}}, {}};
            auto active_auth  = eosio::chain::authority{1, {{txn_text_receiver_C_pub_key, 1}}, {}};

            trx.actions.emplace_back(vector<chain::permission_level>{{creator,"active"}}, newaccount{creator, newaccountC, owner_auth, active_auth});
            }

            trx.expiration = cc.head_block_time() + fc::seconds(30);
            trx.set_reference_block(cc.head_block_id());
            trx.sign(creator_priv_key, chainid);
            trxs.emplace_back(std::move(trx));
         }

         //set txn.test.t contract to eosio.token & initialize it
         {
            signed_transaction trx;

            vector<uint8_t> wasm = contracts::eosio_token_wasm();

            setcode handler;
            handler.account = newaccountC;
            handler.code.assign(wasm.begin(), wasm.end());

            trx.actions.emplace_back( vector<chain::permission_level>{{newaccountC,"active"}}, handler);

            {
               setabi handler;
               handler.account = newaccountC;
               handler.abi = fc::raw::pack(json::from_string(contracts::eosio_token_abi().data()).as<abi_def>());
               trx.actions.emplace_back( vector<chain::permission_level>{{newaccountC,"active"}}, handler);
            }

            {
               action act;
               act.account = N(txn.test.t);
               act.name = N(create);
               act.authorization = vector<permission_level>{{newaccountC,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("create", fc::json::from_string("{\"issuer\":\"txn.test.t\",\"maximum_supply\":\"1000000000.0000 CUR\"}}"), abi_serializer_max_time);
               trx.actions.push_back(act);
            }
            {
               action act;
               act.account = N(txn.test.t);
               act.name = N(issue);
               act.authorization = vector<permission_level>{{newaccountC,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("issue", fc::json::from_string("{\"to\":\"txn.test.t\",\"quantity\":\"60000.0000 CUR\",\"memo\":\"\"}"), abi_serializer_max_time);
               trx.actions.push_back(act);
            }
            {
               action act;
               act.account = N(txn.test.t);
               act.name = N(transfer);
               act.authorization = vector<permission_level>{{newaccountC,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("transfer", fc::json::from_string("{\"from\":\"txn.test.t\",\"to\":\"txn.test.a\",\"quantity\":\"20000.0000 CUR\",\"memo\":\"\"}"), abi_serializer_max_time);
               trx.actions.push_back(act);
            }
            {
               action act;
               act.account = N(txn.test.t);
               act.name = N(transfer);
               act.authorization = vector<permission_level>{{newaccountC,config::active_name}};
               act.data = eosio_token_serializer.variant_to_binary("transfer", fc::json::from_string("{\"from\":\"txn.test.t\",\"to\":\"txn.test.b\",\"quantity\":\"20000.0000 CUR\",\"memo\":\"\"}"), abi_serializer_max_time);
               trx.actions.push_back(act);
            }

            trx.expiration = cc.head_block_time() + fc::seconds(30);
            trx.set_reference_block(cc.head_block_id());
            trx.max_net_usage_words = 5000;
            trx.sign(txn_test_receiver_C_priv_key, chainid);
            trxs.emplace_back(std::move(trx));
         }
      } catch (const fc::exception& e) {
         next(e.dynamic_copy_exception());
         return;
      }

      push_transactions(std::move(trxs), next);
   }

   void start_generation(const std::string& salt, const uint64_t& period, const uint64_t& batch_size) {
      if(running)
         throw fc::exception(fc::invalid_operation_exception_code);
      if(period < 1 || period > 2500)
         throw fc::exception(fc::invalid_operation_exception_code);
      if(batch_size < 1 || batch_size > 250)
         throw fc::exception(fc::invalid_operation_exception_code);
      if(batch_size & 1)
         throw fc::exception(fc::invalid_operation_exception_code);

      running = true;

      controller& cc = app().get_plugin<chain_plugin>().chain();
      auto abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();
      abi_serializer eosio_token_serializer{fc::json::from_string(contracts::eosio_token_abi().data()).as<abi_def>(), abi_serializer_max_time};
      //create the actions here
      act_a_to_b.account = N(txn.test.t);
      act_a_to_b.name = N(transfer);
      act_a_to_b.authorization = vector<permission_level>{{name("txn.test.a"),config::active_name}};
      act_a_to_b.data = eosio_token_serializer.variant_to_binary("transfer", 
                                                                  fc::json::from_string(fc::format_string("{\"from\":\"txn.test.a\",\"to\":\"txn.test.b\",\"quantity\":\"1.0000 CUR\",\"memo\":\"${l}\"}", 
                                                                  fc::mutable_variant_object()("l", salt))),
                                                                  abi_serializer_max_time);

      act_b_to_a.account = N(txn.test.t);
      act_b_to_a.name = N(transfer);
      act_b_to_a.authorization = vector<permission_level>{{name("txn.test.b"),config::active_name}};
      act_b_to_a.data = eosio_token_serializer.variant_to_binary("transfer", 
                                                                  fc::json::from_string(fc::format_string("{\"from\":\"txn.test.b\",\"to\":\"txn.test.a\",\"quantity\":\"1.0000 CUR\",\"memo\":\"${l}\"}", 
                                                                  fc::mutable_variant_object()("l", salt))),
                                                                  abi_serializer_max_time);

      timer_timeout = period;
      batch = batch_size/2;
      nonce_prefix = 0;
      cc._count_blocks = 0;
      cc._count_txns = 0;
      cc._count_accepted_txns = 0;
      cc._count_rejected_txns = 0;
      cc.t_process_transaction = fc::microseconds(0);
      cc.t_sig_transaction = fc::microseconds(0);

      gen_ioc = std::make_shared<boost::asio::io_context>();
      gen_ioc_work.emplace( boost::asio::make_work_guard(*gen_ioc) );
      thread_pool.emplace( thread_pool_size );
      for( uint16_t i = 0; i < thread_pool_size; i++ )
         boost::asio::post( *thread_pool, [ioc = gen_ioc]() { ioc->run(); } );
      timer = std::make_shared<boost::asio::high_resolution_timer>(*gen_ioc);

      ilog("Started transaction test plugin; generating ${p} transactions every ${m} ms by ${t} load generation threads",
         ("p", batch_size) ("m", period) ("t", thread_pool_size));

      _start_time = fc::time_point::now();

      boost::asio::post( *gen_ioc, [this]() {
         arm_timer(boost::asio::high_resolution_timer::clock_type::now());
      });
   }

   void arm_timer(boost::asio::high_resolution_timer::time_point s) {
      timer->expires_at(s + std::chrono::milliseconds(timer_timeout));
      boost::asio::post( *gen_ioc, [this]() {
         send_transaction([this](const fc::exception_ptr& e){
            if (e) {
               elog("pushing transaction failed: ${e}", ("e", e->to_detail_string()));
               if(running)
                  stop_generation();
            }
         }, nonce_prefix++);
      });
      timer->async_wait([this](const boost::system::error_code& ec) {
         if(!running || ec)
            return;
         arm_timer(timer->expires_at());
      });
   }

   void send_transaction(std::function<void(const fc::exception_ptr&)> next, uint64_t nonce_prefix) {
      std::vector<signed_transaction> trxs;
      trxs.reserve(2*batch);

      try {
         controller& cc = app().get_plugin<chain_plugin>().chain();
         auto chainid = app().get_plugin<chain_plugin>().get_chain_id();

         static fc::crypto::private_key a_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'a')));
         static fc::crypto::private_key b_priv_key = fc::crypto::private_key::regenerate(fc::sha256(std::string(64, 'b')));

         static uint64_t nonce = static_cast<uint64_t>(fc::time_point::now().sec_since_epoch()) << 32;

         uint32_t reference_block_num = cc.last_irreversible_block_num();
         if (txn_reference_block_lag >= 0) {
            reference_block_num = cc.head_block_num();
            if (reference_block_num <= (uint32_t)txn_reference_block_lag) {
               reference_block_num = 0;
            } else {
               reference_block_num -= (uint32_t)txn_reference_block_lag;
            }
         }

         block_id_type reference_block_id = cc.get_block_id_for_num(reference_block_num);

         for(unsigned int i = 0; i < batch; ++i) {
         {
         signed_transaction trx;
         trx.actions.push_back(act_a_to_b);
         trx.context_free_actions.emplace_back(action({}, config::null_account_name, "nonce", fc::raw::pack( std::to_string(nonce_prefix)+std::to_string(nonce++) )));
         trx.set_reference_block(reference_block_id);
         trx.expiration = cc.head_block_time() + fc::seconds(30);
         trx.max_net_usage_words = 100;
         trx.sign(a_priv_key, chainid);
         trxs.emplace_back(std::move(trx));
         }

         {
         signed_transaction trx;
         trx.actions.push_back(act_b_to_a);
         trx.context_free_actions.emplace_back(action({}, config::null_account_name, "nonce", fc::raw::pack( std::to_string(nonce_prefix)+std::to_string(nonce++) )));
         trx.set_reference_block(reference_block_id);
         trx.expiration = cc.head_block_time() + fc::seconds(30);
         trx.max_net_usage_words = 100;
         trx.sign(b_priv_key, chainid);
         trxs.emplace_back(std::move(trx));
         }
         }
      } catch ( const fc::exception& e ) {
         next(e.dynamic_copy_exception());
      }

      push_transactions(std::move(trxs), next);
   }

   void stop_generation() {
      if(!running)
         throw fc::exception(fc::invalid_operation_exception_code);
      timer->cancel();
      running = false;
      if( gen_ioc_work )
         gen_ioc_work->reset();
      if( gen_ioc )
         gen_ioc->stop();
      if( thread_pool ) {
         thread_pool->join();
         thread_pool->stop();
      }
      auto elapsed_ms = (fc::time_point::now() - _start_time).count()/1000;
      double blocks = (double)elapsed_ms / 500; // 0.5 sec per block
      controller& cc = app().get_plugin<chain_plugin>().chain();

      ilog("Stopping transaction generation test");

      ilog("${d} transactions executed for ${s} ms (${tps} TPS)",
         ("d", _txcount) ("s", elapsed_ms) ("tps",(int)(((double)_txcount/elapsed_ms)*1000)));
      ilog("${t} transactions (${at},${rt}) produced in ${b} blocks (${tps} TPS)",
         ("t",cc._count_txns) ("at",cc._count_accepted_txns) ("rt",cc._count_rejected_txns) ("b",cc._count_blocks) ("tps",cc._count_txns*2 / cc._count_blocks) );
      ilog("process_transaction ${p} ms/block, sig_transaction ${s} ms/block, billed CPU usage ${t} us/transaction ",
         ("p", (int)((double)cc.t_process_transaction.count()/1000/blocks) ) ("s", (int)((double)cc.t_sig_transaction.count()/1000/blocks) )
         ("t", _total_us / (double)_txcount) );

      _txcount = _total_us = 0;
   }

   bool running{false};

   unsigned timer_timeout;
   unsigned batch;
   uint64_t nonce_prefix;

   action act_a_to_b;
   action act_b_to_a;

   int32_t txn_reference_block_lag;
};

txn_test_gen_plugin::txn_test_gen_plugin() {}
txn_test_gen_plugin::~txn_test_gen_plugin() {}

void txn_test_gen_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
      ("txn-reference-block-lag", bpo::value<int32_t>()->default_value(0), "Lag in number of blocks from the head block when selecting the reference block for transactions (-1 means Last Irreversible Block)")
      ("txn-test-gen-threads", bpo::value<uint16_t>()->default_value(2), "Number of worker threads in txn_test_gen thread pool")
   ;
}

void txn_test_gen_plugin::plugin_initialize(const variables_map& options) {
   try {
      my.reset( new txn_test_gen_plugin_impl );
      my->txn_reference_block_lag = options.at( "txn-reference-block-lag" ).as<int32_t>();
      my->thread_pool_size = options.at( "txn-test-gen-threads" ).as<uint16_t>();
      EOS_ASSERT( my->thread_pool_size > 0, chain::plugin_config_exception,
                  "txn-test-gen-threads ${num} must be greater than 0", ("num", my->thread_pool_size) );
   } FC_LOG_AND_RETHROW()
}

void txn_test_gen_plugin::plugin_startup() {
   app().get_plugin<http_plugin>().add_api({
      CALL_ASYNC(txn_test_gen, my, create_test_accounts, INVOKE_ASYNC_R_R(my, create_test_accounts, std::string, std::string), 200),
      CALL(txn_test_gen, my, stop_generation, INVOKE_V_V(my, stop_generation), 200),
      CALL(txn_test_gen, my, start_generation, INVOKE_V_R_R_R(my, start_generation, std::string, uint64_t, uint64_t), 200)
   });
}

void txn_test_gen_plugin::plugin_shutdown() {
   try {
      my->stop_generation();
   }
   catch(fc::exception e) {
   }
}

}
