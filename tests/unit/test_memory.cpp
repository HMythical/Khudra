// Phase 5 acceptance: interleaved GC/manual work with zero leaks, zero
// premature collection, and pin errors raised correctly.
#include "test_harness.h"

#include <cstring>
#include <string>

#include "bytecode/module.h"
#include "compiler.h"
#include "manual/arena.h"
#include "vm/vm.h"

extern "C" {
#include "alloc.h"
}

namespace {

// Compiles and runs a program, with the collector tuned so a short test still
// exercises several cycles.
struct MemoryRun {
    bool compiled = false;
    bool ran = false;
    std::string output;
    std::string diagnostics;
    std::string runtime_error;
    std::size_t gc_cycles = 0;
    std::size_t reclaimed = 0;
    std::size_t managed_left = 0;
    std::size_t manual_left = 0;
    std::size_t manual_bytes_leaked = 0;
};

MemoryRun run_program(const std::string& source, std::size_t gc_threshold = 4096) {
    MemoryRun result;
    std::size_t manual_before = khu_manual_live_bytes();

    khu::Compiler compiler;
    std::uint32_t file = compiler.add_buffer("mem.khu", source);
    khu::bytecode::Module module;
    result.compiled = compiler.compile(file, module);
    result.diagnostics = compiler.diagnostics().render();
    if (!result.compiled) return result;

    {
        khu::vm::Vm vm(module);
        vm.set_output_sink(&result.output);
        vm.prepare();
        vm.collector().set_threshold(gc_threshold);
        result.ran = vm.run();
        result.runtime_error = vm.error();
        result.gc_cycles = vm.collector().cycles();
        result.reclaimed = vm.collector().total_reclaimed();
        result.managed_left = vm.heap().managed_count();
        result.manual_left = vm.heap().manual_count();
    }

    // Every manual byte belongs to an arena block, and dropping the VM drops
    // the arenas.
    std::size_t manual_after = khu_manual_live_bytes();
    result.manual_bytes_leaked = manual_after > manual_before ? manual_after - manual_before : 0;
    return result;
}

void check_program(const std::string& source, const std::string& expected,
                   std::size_t gc_threshold = 4096) {
    MemoryRun result = run_program(source, gc_threshold);
    if (!result.compiled) {
        KHU_FAIL("did not compile:\n" + result.diagnostics);
        return;
    }
    if (!result.ran) {
        KHU_FAIL("runtime error:\n" + result.runtime_error);
        return;
    }
    KHU_CHECK_EQ(result.output, expected);
    KHU_CHECK_EQ(result.manual_bytes_leaked, static_cast<std::size_t>(0));
}

// A managed list plus a summing walk; several tests build on it.
const char* kListClasses =
    "public class Box {\n"
    "    public int32 value = 0;\n"
    "    public Box next = null;\n"
    "}\n";

}  // namespace

// ---------------------------------------------------------------------------
// Manual arenas
// ---------------------------------------------------------------------------

KHU_TEST(arena, reuses_released_chunks) {
    std::size_t before = khu_manual_live_bytes();
    {
        khu::vm::ManualArena arena(64, 4);
        void* a = arena.allocate();
        void* b = arena.allocate();
        KHU_CHECK(a != nullptr);
        KHU_CHECK(b != nullptr);
        KHU_CHECK_NE(a, b);
        KHU_CHECK_EQ(arena.live_chunks(), static_cast<std::size_t>(2));

        arena.release(a);
        KHU_CHECK_EQ(arena.live_chunks(), static_cast<std::size_t>(1));
        // The freed chunk comes straight back rather than growing the arena.
        void* c = arena.allocate();
        KHU_CHECK_EQ(c, a);
        KHU_CHECK_EQ(arena.block_count(), static_cast<std::size_t>(1));
    }
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
}

KHU_TEST(arena, grows_by_blocks_and_hands_back_zeroed_memory) {
    std::size_t before = khu_manual_live_bytes();
    {
        khu::vm::ManualArena arena(32, 4);
        khu::util::Array<void*> chunks;
        for (int i = 0; i < 10; ++i) {
            void* chunk = arena.allocate();
            KHU_CHECK(chunk != nullptr);
            if (!chunk) break;
            auto* bytes = static_cast<unsigned char*>(chunk);
            for (int b = 0; b < 32; ++b) KHU_CHECK_EQ(static_cast<int>(bytes[b]), 0);
            std::memset(chunk, 0xff, 32);
            chunks.push(chunk);
        }
        KHU_CHECK(arena.block_count() >= 3);
        KHU_CHECK_EQ(arena.live_chunks(), static_cast<std::size_t>(10));

        // A reused chunk is zeroed again, so a stale header can never be read.
        arena.release(chunks[0]);
        auto* reused = static_cast<unsigned char*>(arena.allocate());
        KHU_CHECK_EQ(static_cast<int>(reused[0]), 0);
    }
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
}

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

KHU_TEST(gc, reclaims_unreachable_objects) {
    MemoryRun result = run_program(
        std::string(kListClasses) +
        "public class T {\n"
        "    func churn(int32 rounds) {\n"
        "        int32 i = 0;\n"
        "        while (i < rounds) {\n"
        "            Box junk = Box();\n"
        "            junk.value = i;\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "    }\n"
        "    func main() {\n"
        "        this.churn(2000);\n"
        "        io.printLine(\"done\");\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(result.ran);
    KHU_CHECK(result.gc_cycles > 0);
    // 2000 boxes were allocated; almost all of them are garbage.
    KHU_CHECK(result.reclaimed > 1000);
    KHU_CHECK(result.managed_left < 100);
}

KHU_TEST(gc, never_collects_something_still_reachable) {
    // The list hangs off a field of the entry object, so every element must
    // survive every cycle the churn triggers.
    check_program(
        std::string(kListClasses) +
        "public class T {\n"
        "    public Box kept = null;\n"
        "\n"
        "    func build(int32 count) -> Box {\n"
        "        Box head = null;\n"
        "        int32 i = 0;\n"
        "        while (i < count) {\n"
        "            Box fresh = Box();\n"
        "            fresh.value = i;\n"
        "            fresh.next = head;\n"
        "            head = fresh;\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        return head;\n"
        "    }\n"
        "\n"
        "    func total(Box list) -> int32 {\n"
        "        int32 sum = 0;\n"
        "        Box cursor = list;\n"
        "        while (cursor != null) {\n"
        "            sum = khuStdMath.add(sum, cursor.value);\n"
        "            cursor = cursor.next;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "\n"
        "    func churn(int32 rounds) {\n"
        "        int32 i = 0;\n"
        "        while (i < rounds) {\n"
        "            Box junk = Box();\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "    }\n"
        "\n"
        "    func main() {\n"
        "        this.kept = this.build(50);\n"
        "        this.churn(3000);\n"
        "        io.printLine(this.total(this.kept));\n"
        "        this.churn(3000);\n"
        "        io.printLine(this.total(this.kept));\n"
        "    }\n"
        "}\n",
        "1225\n1225\n");
}

KHU_TEST(gc, keeps_a_half_materialized_object_alive) {
    // A Procedures block that allocates can trigger a cycle while the object
    // being materialized is reachable only from the engine, not from the stack.
    check_program(
        std::string(kListClasses) +
        "public class Heavy {\n"
        "    public int32 marker = 0;\n"
        "    private Procedures {\n"
        "        int32 i = 0;\n"
        "        while (i < 500) {\n"
        "            Box junk = Box();\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "    }\n"
        "    public Heavy() {\n"
        "        this.marker = 99;\n"
        "    }\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        Heavy h = Heavy();\n"
        "        io.printLine(h.marker);\n"
        "    }\n"
        "}\n",
        "99\n", 1024);
}

// ---------------------------------------------------------------------------
// Cross-strategy references
// ---------------------------------------------------------------------------

KHU_TEST(gc, roots_managed_objects_held_by_manual_owners) {
    // manual -> managed: a manual object is outside the collected heap, so what
    // it points at has to be traced from it (docs/memory-model.md, 4.1).
    check_program(
        std::string(kListClasses) +
        "public class Owner {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public Box held = null;\n"
        "}\n"
        "public class T {\n"
        "    func churn(int32 rounds) {\n"
        "        int32 i = 0;\n"
        "        while (i < rounds) {\n"
        "            Box junk = Box();\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "    }\n"
        "    func main() {\n"
        "        Owner owner = Owner();\n"
        "        Box treasure = Box();\n"
        "        treasure.value = 4242;\n"
        "        owner.held = treasure;\n"
        "        this.churn(3000);\n"
        "        io.printLine(owner.held.value);\n"
        "        free(owner);\n"
        "    }\n"
        "}\n",
        "4242\n");
}

KHU_TEST(pins, block_a_free_while_a_managed_slot_still_holds_the_object) {
    MemoryRun result = run_program(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "}\n"
        "public class Holder {\n"
        "    public Node head = null;\n"
        "    func attach(Node n) { this.head = n; }\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        this.attach(n);\n"
        "        free(n);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    // The message has to name the slot, not just report a count.
    KHU_CHECK_CONTAINS(result.runtime_error, "it is still held by 'Holder.head'");
    KHU_CHECK_CONTAINS(result.runtime_error, "clear the reference before releasing the object");
}

KHU_TEST(pins, release_when_the_managed_slot_is_overwritten) {
    check_program(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "}\n"
        "public class Holder {\n"
        "    public Node head = null;\n"
        "    func attach(Node n) { this.head = n; }\n"
        "    func detach() { this.head = null; }\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        n.value = 5;\n"
        "        this.attach(n);\n"
        "        this.detach();\n"
        "        free(n);\n"
        "        io.printLine(\"released\");\n"
        "    }\n"
        "}\n",
        "released\n");
}

KHU_TEST(pins, are_dropped_when_the_holding_managed_object_is_collected) {
    // The sweep walks a dying object's reference map and decrements the pins it
    // was holding, so the manual object becomes releasable again.
    check_program(
        std::string(kListClasses) +
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "}\n"
        "public class Holder {\n"
        "    public Node head = null;\n"
        "}\n"
        "public class T {\n"
        "    func attach(Node n) {\n"
        "        Holder temporary = Holder();\n"
        "        temporary.head = n;\n"
        "    }\n"
        "    func churn(int32 rounds) {\n"
        "        int32 i = 0;\n"
        "        while (i < rounds) {\n"
        "            Box junk = Box();\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "    }\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        this.attach(n);\n"
        "        this.churn(3000);\n"
        "        free(n);\n"
        "        io.printLine(\"released after the holder was collected\");\n"
        "    }\n"
        "}\n",
        "released after the holder was collected\n");
}

KHU_TEST(manual, reports_a_double_release_at_runtime) {
    MemoryRun result = run_program(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "}\n"
        "public class T {\n"
        "    func release(Node n) { free(n); }\n"
        "    func main() {\n"
        "        Node n = Node();\n"
        "        this.release(n);\n"
        "        this.release(n);\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.compiled);
    KHU_CHECK(!result.ran);
    KHU_CHECK_CONTAINS(result.runtime_error, "already released");
}

// ---------------------------------------------------------------------------
// Interleaved stress
// ---------------------------------------------------------------------------

KHU_TEST(memory, survives_interleaved_gc_and_manual_traffic) {
    MemoryRun result = run_program(
        std::string(kListClasses) +
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "    public Box payload = null;\n"
        "}\n"
        "public class Holder {\n"
        "    public Node pinned = null;\n"
        "    public Box kept = null;\n"
        "}\n"
        "public class T {\n"
        "    public Holder holder = null;\n"
        "    public int32 checksum = 0;\n"
        "\n"
        "    func round(int32 seed) {\n"
        "        // A manual node carrying a managed payload: the payload is\n"
        "        // rooted through the node, and the node is pinned by the\n"
        "        // managed holder.\n"
        "        Node node = Node();\n"
        "        node.value = seed;\n"
        "        Box payload = Box();\n"
        "        payload.value = khuStdMath.multiply(seed, 2);\n"
        "        node.payload = payload;\n"
        "        this.holder.pinned = node;\n"
        "\n"
        "        // Garbage, to keep the collector busy while both are live.\n"
        "        int32 i = 0;\n"
        "        while (i < 40) {\n"
        "            Box junk = Box();\n"
        "            junk.value = i;\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "\n"
        "        this.checksum = khuStdMath.add(this.checksum, node.payload.value);\n"
        "        this.holder.pinned = null;\n"
        "        free(node);\n"
        "    }\n"
        "\n"
        "    func main() {\n"
        "        this.holder = Holder();\n"
        "        this.holder.kept = Box();\n"
        "        this.holder.kept.value = 7;\n"
        "\n"
        "        int32 round = 0;\n"
        "        while (round < 200) {\n"
        "            this.round(round);\n"
        "            round = khuStdMath.add(round, 1);\n"
        "        }\n"
        "\n"
        "        io.printLine(this.checksum);\n"
        "        io.printLine(this.holder.kept.value);\n"
        "    }\n"
        "}\n",
        2048);

    if (!result.compiled) {
        KHU_FAIL("did not compile:\n" + result.diagnostics);
        return;
    }
    if (!result.ran) {
        KHU_FAIL("runtime error:\n" + result.runtime_error);
        return;
    }

    // 2 * (0 + 1 + ... + 199) = 39800.
    KHU_CHECK_EQ(result.output, std::string("39800\n7\n"));
    // The collector really ran, and every manual node was released.
    KHU_CHECK(result.gc_cycles > 1);
    KHU_CHECK(result.reclaimed > 100);
    KHU_CHECK_EQ(result.manual_left, static_cast<std::size_t>(0));
    // Zero leaks: every arena block went back to the C runtime.
    KHU_CHECK_EQ(result.manual_bytes_leaked, static_cast<std::size_t>(0));
}

KHU_TEST(memory, leaves_no_manual_bytes_behind_after_a_program_runs) {
    std::size_t before = khu_manual_live_bytes();
    MemoryRun result = run_program(
        "public class Node {\n"
        "    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();\n"
        "    public int32 value = 0;\n"
        "}\n"
        "public class T {\n"
        "    func main() {\n"
        "        int32 i = 0;\n"
        "        while (i < 500) {\n"
        "            Node n = Node();\n"
        "            n.value = i;\n"
        "            free(n);\n"
        "            i = khuStdMath.add(i, 1);\n"
        "        }\n"
        "        // Deliberately leaked by the program: released by the arena\n"
        "        // when the VM shuts down, not silently forgotten.\n"
        "        Node stray = Node();\n"
        "        io.printLine(\"done\");\n"
        "    }\n"
        "}\n");
    KHU_CHECK(result.ran);
    KHU_CHECK_EQ(result.output, std::string("done\n"));
    KHU_CHECK_EQ(result.manual_bytes_leaked, static_cast<std::size_t>(0));
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
}
