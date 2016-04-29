#include <glog/logging.h>

#include <thread>

#include "input_assigner.hpp"
#include "mock_input_assigner_strategy.hpp"
#include "mock_input_state_cache.hpp"
#include "mock_io_runner.hpp"
#include "mock_port_reader.hpp"

#define EXPECT_DETH(a, b) { std::cerr << "Disregard the following warning:"; EXPECT_DEATH(a, b); }

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {
  inline mesos::ContainerID container_id(const std::string& id) {
    mesos::ContainerID cid;
    cid.set_value(id);
    return cid;
  }
  inline mesos::ExecutorInfo exec_info(const std::string& fid, const std::string& eid) {
    mesos::ExecutorInfo ei;
    ei.mutable_framework_id()->set_value(fid);
    ei.mutable_executor_id()->set_value(eid);
    return ei;
  }
  inline Try<stats::UDPEndpoint> try_endpoint(const stats::UDPEndpoint& endpoint) {
    return endpoint;
  }
  inline Try<stats::UDPEndpoint> try_endpoint(const std::string& host, size_t port) {
    stats::UDPEndpoint endpoint(host, port);
    return try_endpoint(endpoint);
  }
  inline mesos::slave::ContainerState container_state(
      const std::string& cid, const std::string& fid, const std::string& eid) {
    mesos::slave::ContainerState state;
    *state.mutable_container_id() = container_id(cid);
    *state.mutable_executor_info() = exec_info(fid, eid);
    return state;
  }

  void register_get_unregister(stats::InputAssigner& assigner, size_t id) {
    std::cout << "thread " << id << " start" << std::endl;
    std::ostringstream oss;
    oss << "c" << id;
    mesos::ContainerID cid = container_id(oss.str());

    oss.clear();
    oss << "f" << id;
    std::string fid = oss.str();
    oss.clear();
    oss << "e" << id;
    mesos::ExecutorInfo einfo = exec_info(fid, oss.str());

    EXPECT_FALSE(assigner.register_container(cid, einfo).isError()) << "thread " << id;
    assigner.unregister_container(cid);
    std::cout << "thread " << id << " end" << std::endl;
  }

  // Simple boilerplate utility to simulate IORunner's async scheduler, which just runs the
  // function synchronously
  void execute(std::function<void()> func) {
    func();
  }
}

MATCHER_P(ContainerIdMatch, proto_value, "mesos::ContainerID") {
  return arg.value() == proto_value.value();
}

MATCHER_P(ContainerStrMatch, str_value, "mesos::ContainerID") {
  return arg.value() == str_value;
}

MATCHER_P(ExecInfoMatch, proto_value, "mesos::ExecutorInfo") {
  return arg.executor_id().value() == proto_value.executor_id().value()
    && arg.framework_id().value() == proto_value.framework_id().value();
}

class InputAssignerTests : public ::testing::Test {
 public:
  InputAssignerTests()
    : mock_runner(new MockIORunner),
      mock_state_cache(new MockInputStateCache),
      mock_strategy(new MockInputAssignerStrategy) { }

 protected:
  std::shared_ptr<MockIORunner> mock_runner;
  std::shared_ptr<MockInputStateCache> mock_state_cache;
  std::shared_ptr<MockInputAssignerStrategy> mock_strategy;
};

TEST_F(InputAssignerTests, init_fails) {
  stats::InputAssigner input_assigner;
  EXPECT_DETH(input_assigner.register_container(container_id("hi"), exec_info("hey", "hello")),
      ".*init\\(\\) wasn't called before register_container\\(\\).*");
  std::list<mesos::slave::ContainerState> states;
  EXPECT_DETH(input_assigner.recover_containers(states),
      ".*init\\(\\) wasn't called before recover_containers\\(\\).*");
  EXPECT_DETH(input_assigner.unregister_container(container_id("hi")),
      ".*init\\(\\) wasn't called before unregister_container\\(\\).*");

  input_assigner.init(mock_runner, mock_state_cache, mock_strategy);

  EXPECT_DETH(input_assigner.init(mock_runner, mock_state_cache, mock_strategy),
      ".*init\\(\\) was called twice.*");
}

TEST_F(InputAssignerTests, multithread) {
  stats::InputAssigner input_assigner;
  input_assigner.init(mock_runner, mock_state_cache, mock_strategy);
  EXPECT_CALL(*mock_runner, dispatch(_)).WillRepeatedly(Invoke(execute));
  EXPECT_CALL(*mock_strategy, register_container(_, _))
    .WillRepeatedly(Return(try_endpoint("ignored", 0)));
  EXPECT_CALL(*mock_state_cache, add_container(_, _)).Times(AtLeast(1));
  EXPECT_CALL(*mock_strategy, unregister_container(_)).WillRepeatedly(Return());
  EXPECT_CALL(*mock_state_cache, remove_container(_)).Times(AtLeast(1));

  std::list<std::thread*> thread_ptrs;
  for (int i = 0; i < 250; ++i) {
    //Note: Tried getting AND resetting in each thread, but this led to glogging races.
    //      That behavior isn't supported anyway.
    thread_ptrs.push_back(new std::thread(
            std::bind(register_get_unregister, std::ref(input_assigner), i)));
  }
  for (std::thread* thread : thread_ptrs) {
    thread->join();
    delete thread;
  }
  thread_ptrs.clear();
}

TEST_F(InputAssignerTests, recovery) {
  stats::InputAssigner input_assigner;
  input_assigner.init(mock_runner, mock_state_cache, mock_strategy);

  // Permutations:
  //   | recovery | disk || expect result
  // --+----------+------+------------++--------------------------
  // 1 | Y        | Y    || insert with disk endpoint (#1)
  // 2 | Y        | N    || register without endpoint (#3)
  // 3 | N        | Y    || remove/unregister (#2)
  // 4 | N        | N    || (doesn't exist!)

  EXPECT_CALL(*mock_runner, dispatch(_)).WillRepeatedly(Invoke(execute));

  std::list<mesos::slave::ContainerState> recover_input; // Y**

  recover_input.push_back(container_state("YY", "fid1", "eid1"));
  recover_input.push_back(container_state("YN", "fid2", "eid2"));

  stats::container_id_map<stats::UDPEndpoint> disk_input; // *Y*

  disk_input.insert({container_id("YY"), stats::UDPEndpoint("host1", 1)});
  disk_input.insert({container_id("NY"), stats::UDPEndpoint("host3", 2)});

  // set up expected outcomes when we call recover:

  EXPECT_CALL(*mock_state_cache, get_containers()).WillOnce(Return(disk_input));
  const std::string path("SOME PATH");
  EXPECT_CALL(*mock_state_cache, path()).WillOnce(ReturnRef(path));

  // 1: fresh registration on cached port 1
  EXPECT_CALL(*mock_strategy, insert_container(
          ContainerStrMatch("YY"),
          ExecInfoMatch(exec_info("fid1", "eid1")),
          stats::UDPEndpoint("host1", 1)));

  // 2: new registration against any location (just makes one up)
  EXPECT_CALL(*mock_strategy, register_container(
          ContainerStrMatch("YN"), ExecInfoMatch(exec_info("fid2", "eid2"))))
    .WillOnce(Return(try_endpoint("host2", 2)));
  EXPECT_CALL(*mock_state_cache, add_container(
    ContainerStrMatch("YN"), stats::UDPEndpoint("host2", 2)));

  // 3: unregistered
  EXPECT_CALL(*mock_strategy, unregister_container(ContainerStrMatch("NY")));
  EXPECT_CALL(*mock_state_cache, remove_container(ContainerStrMatch("NY")));

  input_assigner.recover_containers(recover_input);
}

// no port_range_multithread test: mock would need to pass through the range pool's returned ports

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  // avoid non-threadsafe logging code for these tests
  //FLAGS_logtostderr = 1;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
