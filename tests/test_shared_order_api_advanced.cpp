// tests/test_shared_order_api_advanced.cpp
//
// SharedOrderApi의 고급 멀티스레드 검증 테스트
//
// 목적 - 여러 스레드가 동시에 UpbitExchangeRestClient를 호출해도 mutex를 통해 직렬화하여
// UpbitExchangeRestClient를 단일 스레드처럼 안전하게 동작하도록 만든다
// 
// 검증 항목:
// 1. 직렬화 검증: 동시 실행 수가 1을 초과하지 않음
// 2. 예외 안전성: 한 스레드의 에러가 다른 스레드에 영향 없음
// 3. 부하 테스트: 다수 스레드에서 장시간 실행

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <algorithm>

#include "api/upbit/SharedOrderApi.h"
#include "api/upbit/UpbitExchangeRestClient.h"
#include "api/rest/RestClient.h"
#include "api/auth/UpbitJwtSigner.h"
#include "core/domain/OrderRequest.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

using namespace std::chrono_literals;

namespace test {

    // 테스트 1: 직렬화 검증
    // - mutex가 제대로 작동하면 동시 실행 수가 절대 1을 초과하지 않음
    void testSerializationProof(std::shared_ptr<api::upbit::SharedOrderApi> api)
    {
        std::cout << "\n[TEST] Serialization proof (LOCK-INTERNAL instrumentation)\n";
        api->debugResetInFlight();

		// 스레드 개수 설정 (5개 스레드, 각 스레드당 5회 호출)
        const int num_threads = 5;
        const int calls_per_thread = 5;

        // 스레드 생성 및 개수 할당
        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        // 스레드 당 getMyAccount를 5번 호출
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < calls_per_thread; ++i) {
                    (void)api->getMyAccount();
                }
                });
        }
        for (auto& th : threads) th.join(); // 스레드 5개 모두 끝날 때까지 종료 대기
        
		// 내부에서 기록된 최대 동시 실행 수 확인
        const int max_inflight = api->debugMaxInFlight();
        std::cout << "  SharedOrderApi max_in_flight (inside mutex): " << max_inflight << "\n";

        if (max_inflight == 1) {
            std::cout << "  [PASS] Serialization verified (max_in_flight == 1)\n";
        }
        else {
            std::cout << "  [FAIL] Serialization FAILED! max_in_flight: " << max_inflight << " (expected: 1)\n";
        }
    }

    // 테스트 2: 예외 안전성
    // - 잘못된 요청을 보내는 스레드와 정상 요청을 보내는 스레드 혼합
    void testExceptionSafety(std::shared_ptr<api::upbit::SharedOrderApi> api)
    {
        std::cout << "\nException safety (mix failures + normal calls)\n";

        // 멀티스레드에서 동시에 증가하므로 atomic 사용한 결과 카운터
        std::atomic<int> good_success{ 0 };
        std::atomic<int> bad_errors{ 0 };

        auto goodWorker = [&]() {
            for (int i = 0; i < 10; ++i) {
                // api를 호출해서
                auto result = api->getMyAccount();
                if (std::holds_alternative<core::Account>(result)) { // Account를 가져오면 성공
                    good_success.fetch_add(1);
                }
                std::this_thread::sleep_for(20ms);
            }
            };

        auto badWorker = [&]() {
            for (int i = 0; i < 10; ++i) {
                // 의도적으로 잘못된 요청 생성
                core::OrderRequest bad_req;
                bad_req.market = "";  // 빈 마켓 (에러 발생)
                bad_req.position = core::OrderPosition::BID;
                bad_req.type = core::OrderType::Market;
                bad_req.size = core::AmountSize{ 5000.0 };

                auto result = api->postOrder(bad_req);
                if (std::holds_alternative<api::rest::RestError>(result)) {
                    bad_errors.fetch_add(1);
                }
                std::this_thread::sleep_for(20ms);
            }
            };

        // 스레드 3개를 섞어서 실행
        std::vector<std::thread> threads;
        threads.emplace_back(goodWorker);
        threads.emplace_back(badWorker);
        threads.emplace_back(goodWorker);

        for (auto& t : threads) {
            t.join();   // 완료 대기
        }

		// 결과 출력(atomic은 load로 읽기)
        std::cout << "  Normal-call successes: " << good_success.load() << "\n";
        std::cout << "  Bad-call errors: " << bad_errors.load() << "\n";

        // 테스트 성공 조건
        if (good_success > 0 && bad_errors > 0) {
            std::cout << "  [PASS] Exception safety verified\n";
            std::cout << "  (A failure in one thread does not affect others)\n";
        }
        else {
            std::cout << "  [FAIL] Exception safety FAILED\n";
        }
    }

    // 테스트 3: 부하 테스트
    // - 다수 스레드에서 많은 요청 전송시 전체적으로 안정적으로 동작하는지
    void testLoadTest(std::shared_ptr<api::upbit::SharedOrderApi> api, int num_threads, int calls_per_thread)
    {
        std::cout << "\n[TEST]  ("
            << num_threads << " threads, per " << calls_per_thread << "calls each)\n";

        // 결과 카운터
        std::atomic<int> success{ 0 };
        std::atomic<int> errors{ 0 };

        auto worker = [&]() {
            for (int i = 0; i < calls_per_thread; ++i) {
                auto result = api->getMyAccount();
                if (std::holds_alternative<core::Account>(result)) {
                    success.fetch_add(1);
                }
                else {
                    errors.fetch_add(1);
                }
                std::this_thread::sleep_for(10ms); // 간격을 아주 짧게해 부하를 유발
            }
            };

        // 스레드 생성
        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        auto start = std::chrono::steady_clock::now();

        // 스레드별 동시에 worker 실행
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }

        for (auto& t : threads) {
			t.join(); // 완료 대기
        }

        // 시간 측정
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);

        int total = success.load() + errors.load();
        std::cout << "  total call: " << total << "\n";
        std::cout << "  success: " << success << ""
            << (success * 100.0 / total) << "%)\n"; // 비율로 에러율 판정
        std::cout << "  fail: " << errors << "\n";
        std::cout << "  Elapsed: " << duration.count() << "sec\n";

        // 에러 0 or 성공률 95퍼 초과면 성공
        if (errors == 0 || (success * 100.0 / total) > 95.0) {
            std::cout << "  [PASS] test clear\n";
        }
        else {
            std::cout << "  [WARN] failure is higher \n";
        }
    }

    // 실제 환경 테스트
    bool runAdvancedTests()
    {
        std::cout << "\n=== SharedOrderApi advanced test ===\n";

        // 환경 변수에서 API 키 읽기
        const char* access_key_env = std::getenv("UPBIT_ACCESS_KEY");
        const char* secret_key_env = std::getenv("UPBIT_SECRET_KEY");

        if (!access_key_env || !secret_key_env) {
            std::cerr << "\n[ERROR] UPBIT_ACCESS_KEY, UPBIT_SECRET_KEY error\n";
            std::cerr << "advanced test skip\n";
            return false;
        }

        std::string access_key{ access_key_env };
        std::string secret_key{ secret_key_env };

        boost::asio::io_context ioc;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);

        // RestClient 및 UpbitExchangeRestClient 생성
        api::rest::RestClient rest_client(ioc, ssl_ctx);
        api::auth::UpbitJwtSigner signer(std::move(access_key), std::move(secret_key));

        auto upbit_client = std::make_unique<api::rest::UpbitExchangeRestClient>(
            rest_client, std::move(signer));

        // SharedOrderApi 생성 (upbit_client를 SharedOrderApi로 감싸서)
        auto shared_api = std::make_shared<api::upbit::SharedOrderApi>(std::move(upbit_client));

        // 3개의 테스트 순차 실행
        testSerializationProof(shared_api);
        testExceptionSafety(shared_api);
        testLoadTest(shared_api, 5, 10);

        std::cout << "\n=== advanced test finish ===\n";
        return true;
    }

} // namespace test

int main()
{
    std::cout << "SharedOrderApi Advanced Thread-Safety Test\n";
    std::cout << "==========================================\n";

    test::runAdvancedTests();
    return 0;
}
