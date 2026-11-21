// Benchmark of neojson against cpp-yyjson, yyjson's C API and nlohmann/json.
//
// One executable is built per library (BENCH_LIB_* below), so that no executable links two copies of yyjson. Each
// executable runs the same workloads on the same files and prints one JSON object per line. bench/run.py checks that
// the executables agree on the content of every file and then runs them in alternating rounds.
//
// Workloads:
//   parse   parse the text into the library's document and destroy it
//   read    parse, visit every value (counting and summing them, see Stats), destroy
//   write   serialize a document parsed before the timing (minified)
//   build   copy a document parsed before the timing into a new document through the library's insertion API
//           (strings copied), destroy it
//   lookup  on a document parsed before the timing, read a few fields of every element of an array by key
//           (only on the files that the manifest marks with a lookup path)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(BENCH_LIB_NEOJSON)
#include <neojson/json.hpp>
#elif defined(BENCH_LIB_NLOHMANN)
#include <nlohmann/json.hpp>
#elif defined(BENCH_LIB_CPPYYJSON)
#include <cpp_yyjson.hpp>
#elif defined(BENCH_LIB_YYJSON)
#include <yyjson.h>
#else
#error "define one BENCH_LIB_*"
#endif

namespace {

// What a visit of every value finds. Integers are summed modulo 2^64 so that signed and unsigned values add up the
// same way in every library.
struct Stats {
    std::uint64_t arrays = 0, objects = 0, strings = 0, ints = 0, reals = 0, nulls = 0, bools = 0;
    std::uint64_t array_elements = 0, object_members = 0, string_bytes = 0, int_sum = 0, trues = 0;
    double real_sum = 0;

    void add_int(std::uint64_t v) {
        ++ints;
        int_sum += v;
    }
    void add_real(double v) {
        ++reals;
        real_sum += v;
    }
    void add_bool(bool b) {
        ++bools;
        trues += b ? 1 : 0;
    }

    bool same_counts(const Stats& o) const {
        return arrays == o.arrays && objects == o.objects && strings == o.strings && ints == o.ints &&
               reals == o.reals && nulls == o.nulls && bools == o.bools && array_elements == o.array_elements &&
               object_members == o.object_members && string_bytes == o.string_bytes && int_sum == o.int_sum &&
               trues == o.trues;
    }
    bool operator==(const Stats& o) const {
        const double scale = std::max({1.0, std::fabs(real_sum), std::fabs(o.real_sum)});
        return same_counts(o) && std::fabs(real_sum - o.real_sum) <= 1e-12 * scale;
    }
    std::uint64_t fold() const { return arrays + objects + strings + ints + string_bytes + int_sum; }

    std::string to_json() const {
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "{\"arrays\":%llu,\"objects\":%llu,\"strings\":%llu,\"ints\":%llu,\"reals\":%llu,"
                      "\"nulls\":%llu,\"bools\":%llu,\"array_elements\":%llu,\"object_members\":%llu,"
                      "\"string_bytes\":%llu,\"int_sum\":%llu,\"trues\":%llu,\"real_sum\":%.17g}",
                      (unsigned long long)arrays, (unsigned long long)objects, (unsigned long long)strings,
                      (unsigned long long)ints, (unsigned long long)reals, (unsigned long long)nulls,
                      (unsigned long long)bools, (unsigned long long)array_elements,
                      (unsigned long long)object_members, (unsigned long long)string_bytes,
                      (unsigned long long)int_sum, (unsigned long long)trues, real_sum);
        return buf;
    }
};

// The lookup workload: for every element of the array at `array_key` of the root object, add the length of the
// string at element[object_key][string_key], the integer at element[int_key] and the size of the array at
// element[object_key2][array_key2]. The keys come from the manifest (bench/corpus.json).
struct LookupPath {
    std::string array_key, object_key, string_key, int_key, object_key2, array_key2;
};

#if defined(BENCH_LIB_NEOJSON) || defined(BENCH_LIB_NLOHMANN)

// neojson implements the nlohmann API, so both libraries run the same code.
#if defined(BENCH_LIB_NEOJSON)
using J = neojson::json;
#else
using J = nlohmann::ordered_json;  // neojson keeps the order of insertion, as ordered_json does
#endif

void visit(const J& v, Stats& s) {
    if (v.is_object()) {
        ++s.objects;
        s.object_members += v.size();
        for (auto it = v.cbegin(); it != v.cend(); ++it) {
            s.string_bytes += it.key().size();
            visit(it.value(), s);
        }
    } else if (v.is_array()) {
        ++s.arrays;
        s.array_elements += v.size();
        for (const auto& e : v) visit(e, s);
    } else if (v.is_string()) {
        ++s.strings;
        s.string_bytes += v.get_ptr<const J::string_t*>()->size();
    } else if (v.is_number_unsigned()) {
        s.add_int(*v.get_ptr<const J::number_unsigned_t*>());
    } else if (v.is_number_integer()) {
        s.add_int(static_cast<std::uint64_t>(*v.get_ptr<const J::number_integer_t*>()));
    } else if (v.is_number_float()) {
        s.add_real(*v.get_ptr<const J::number_float_t*>());
    } else if (v.is_boolean()) {
        s.add_bool(*v.get_ptr<const J::boolean_t*>());
    } else {
        ++s.nulls;
    }
}

J build(const J& v) {
    if (v.is_object()) {
        J out = J::object();
        for (auto it = v.cbegin(); it != v.cend(); ++it) out[it.key()] = build(it.value());
        return out;
    }
    if (v.is_array()) {
        J out = J::array();
        for (const auto& e : v) out.push_back(build(e));
        return out;
    }
    return v;
}

struct Lib {
    using Doc = J;
    using Built = J;
    static Doc parse(std::string_view text) { return J::parse(text); }
    static Stats stats(const Doc& d) {
        Stats s;
        visit(d, s);
        return s;
    }
    static std::string write(const Doc& d) { return d.dump(); }
    static Built build(const Doc& d) { return ::build(d); }
    static std::string write_built(const Built& b) { return b.dump(); }
    static std::uint64_t touch(const Doc& d) { return d.size(); }
    static std::uint64_t touch_built(const Built& b) { return b.size(); }
    static std::uint64_t lookup(const Doc& d, const LookupPath& p) {
        std::uint64_t sum = 0;
        for (const auto& e : d[p.array_key]) {
            sum += e[p.object_key][p.string_key].get_ptr<const J::string_t*>()->size();
            sum += e[p.int_key].get<std::uint64_t>();
            sum += e[p.object_key2][p.array_key2].size();
        }
        return sum;
    }
};

#elif defined(BENCH_LIB_CPPYYJSON)

// The visit dispatches with `inspect`, as cpp-yyjson's own read benchmark does.
Stats cpp_stats(const yyjson::reader::value& doc) {
    using namespace yyjson::reader;
    Stats s;
    auto visit = yyjson::overloaded{
        [&](auto& self, const const_value_ref value) -> void { value.inspect(self); },
        [&](auto& self, const_array_ref arr) {
            ++s.arrays;
            s.array_elements += arr.size();
            for (const auto& e : arr) self(e);
        },
        [&](auto& self, const_object_ref obj) {
            ++s.objects;
            s.object_members += obj.size();
            for (const auto& [k, e] : obj) {
                s.string_bytes += k.size();
                self(e);
            }
        },
        [&](auto&, std::string_view str) {
            ++s.strings;
            s.string_bytes += str.size();
        },
        [&](auto&, std::uint64_t n) { s.add_int(n); },
        [&](auto&, std::int64_t n) { s.add_int(static_cast<std::uint64_t>(n)); },
        [&](auto&, double r) { s.add_real(r); },
        [&](auto&, std::nullptr_t) { ++s.nulls; },
        [&](auto&, bool b) { s.add_bool(b); }};
    visit(doc);
    return s;
}

// Calls f with the scalar held by v, converted to the C++ type that cpp-yyjson's mutable classes take.
template <class V, class F>
void with_scalar(const V& v, F&& f) {
    if (auto str = v.as_string()) {
        f(*str);
    } else if (v.is_uint()) {
        f(*v.as_uint());
    } else if (v.is_sint()) {
        f(*v.as_sint());
    } else if (auto r = v.as_real()) {
        f(*r);
    } else if (auto b = v.as_bool()) {
        f(*b);
    } else {
        f(nullptr);
    }
}

template <class Out, class In>
void fill_array(Out& out, const In& in);

// Fills a mutable object in place: an inserted container is filled through the reference that `emplace` returns,
// which cpp-yyjson's documentation recommends over building separate values. Strings are copied (copy_string),
// because neojson and nlohmann/json own their strings too.
template <class Out, class In>
void fill_object(Out& out, const In& in) {
    for (const auto& [k, e] : in) {
        if (auto o = e.as_object()) {
            auto child = out.emplace(k, yyjson::empty_object, yyjson::copy_string);
            fill_object(child, *o);
        } else if (auto a = e.as_array()) {
            auto child = out.emplace(k, yyjson::empty_array, yyjson::copy_string);
            fill_array(child, *a);
        } else {
            with_scalar(e, [&](auto x) { out.emplace(k, x, yyjson::copy_string); });
        }
    }
}

template <class Out, class In>
void fill_array(Out& out, const In& in) {
    for (const auto& e : in) {
        if (auto o = e.as_object()) {
            auto child = out.emplace_back(yyjson::empty_object);
            fill_object(child, *o);
        } else if (auto a = e.as_array()) {
            auto child = out.emplace_back(yyjson::empty_array);
            fill_array(child, *a);
        } else {
            with_scalar(e, [&](auto x) { out.emplace_back(x, yyjson::copy_string); });
        }
    }
}

struct Lib {
    using Doc = yyjson::reader::value;
    // The files of this benchmark have an object or an array at the root.
    struct Built {
        std::optional<yyjson::object> object;
        std::optional<yyjson::array> array;
    };
    static Doc parse(std::string_view text) { return yyjson::read(text); }
    static Stats stats(const Doc& d) { return cpp_stats(d); }
    static std::string write(const Doc& d) {
        auto out = d.write();
        return std::string(out);
    }
    static Built build(const Doc& d) {
        Built b;
        if (auto o = d.as_object()) {
            b.object.emplace();
            fill_object(*b.object, *o);
        } else if (auto a = d.as_array()) {
            b.array.emplace();
            fill_array(*b.array, *a);
        } else {
            throw std::runtime_error("the root is neither an object nor an array");
        }
        return b;
    }
    static std::string write_built(const Built& b) {
        auto out = b.object ? b.object->write() : b.array->write();
        return std::string(out);
    }
    static std::uint64_t touch(const Doc& d) {
        if (auto o = d.as_object()) return o->size();
        return d.as_array()->size();
    }
    static std::uint64_t touch_built(const Built& b) { return b.object ? b.object->size() : b.array->size(); }
    static std::uint64_t lookup(const Doc& d, const LookupPath& p) {
        std::uint64_t sum = 0;
        const auto root = *d.as_object();
        for (const auto& e : *root[p.array_key].as_array()) {
            const auto obj = *e.as_object();
            sum += (*obj[p.object_key].as_object())[p.string_key].as_string()->size();
            sum += *obj[p.int_key].as_uint();
            sum += (*obj[p.object_key2].as_object())[p.array_key2].as_array()->size();
        }
        return sum;
    }
};

#elif defined(BENCH_LIB_YYJSON)

void visit(yyjson_val* v, Stats& s) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_OBJ: {
            ++s.objects;
            s.object_members += yyjson_obj_size(v);
            yyjson_val *key, *val;
            yyjson_obj_iter it = yyjson_obj_iter_with(v);
            while ((key = yyjson_obj_iter_next(&it))) {
                val = yyjson_obj_iter_get_val(key);
                s.string_bytes += yyjson_get_len(key);
                visit(val, s);
            }
            break;
        }
        case YYJSON_TYPE_ARR: {
            ++s.arrays;
            s.array_elements += yyjson_arr_size(v);
            yyjson_val* e;
            yyjson_arr_iter it = yyjson_arr_iter_with(v);
            while ((e = yyjson_arr_iter_next(&it))) visit(e, s);
            break;
        }
        case YYJSON_TYPE_STR:
            ++s.strings;
            s.string_bytes += yyjson_get_len(v);
            break;
        case YYJSON_TYPE_NUM:
            if (yyjson_is_uint(v)) {
                s.add_int(yyjson_get_uint(v));
            } else if (yyjson_is_sint(v)) {
                s.add_int(static_cast<std::uint64_t>(yyjson_get_sint(v)));
            } else {
                s.add_real(yyjson_get_real(v));
            }
            break;
        case YYJSON_TYPE_BOOL:
            s.add_bool(yyjson_get_bool(v));
            break;
        default:
            ++s.nulls;
            break;
    }
}

yyjson_mut_val* copy(yyjson_mut_doc* doc, yyjson_val* v) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_OBJ: {
            yyjson_mut_val* out = yyjson_mut_obj(doc);
            yyjson_val* key;
            yyjson_obj_iter it = yyjson_obj_iter_with(v);
            while ((key = yyjson_obj_iter_next(&it))) {
                yyjson_mut_obj_add(out, yyjson_mut_strncpy(doc, yyjson_get_str(key), yyjson_get_len(key)),
                                   copy(doc, yyjson_obj_iter_get_val(key)));
            }
            return out;
        }
        case YYJSON_TYPE_ARR: {
            yyjson_mut_val* out = yyjson_mut_arr(doc);
            yyjson_val* e;
            yyjson_arr_iter it = yyjson_arr_iter_with(v);
            while ((e = yyjson_arr_iter_next(&it))) yyjson_mut_arr_append(out, copy(doc, e));
            return out;
        }
        case YYJSON_TYPE_STR:
            return yyjson_mut_strncpy(doc, yyjson_get_str(v), yyjson_get_len(v));
        case YYJSON_TYPE_NUM:
            if (yyjson_is_uint(v)) return yyjson_mut_uint(doc, yyjson_get_uint(v));
            if (yyjson_is_sint(v)) return yyjson_mut_sint(doc, yyjson_get_sint(v));
            return yyjson_mut_real(doc, yyjson_get_real(v));
        case YYJSON_TYPE_BOOL:
            return yyjson_mut_bool(doc, yyjson_get_bool(v));
        default:
            return yyjson_mut_null(doc);
    }
}

std::uint64_t as_u64(yyjson_val* v) {
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_sint(v)) return static_cast<std::uint64_t>(yyjson_get_sint(v));
    throw std::runtime_error("not an integer");
}

struct Lib {
    struct DocFree {
        void operator()(yyjson_doc* d) const { yyjson_doc_free(d); }
    };
    struct MutDocFree {
        void operator()(yyjson_mut_doc* d) const { yyjson_mut_doc_free(d); }
    };
    using Doc = std::unique_ptr<yyjson_doc, DocFree>;
    using Built = std::unique_ptr<yyjson_mut_doc, MutDocFree>;
    static Doc parse(std::string_view text) {
        yyjson_read_err err;
        Doc d(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), 0, nullptr, &err));
        if (!d) throw std::runtime_error(err.msg);
        return d;
    }
    static Stats stats(const Doc& d) {
        Stats s;
        visit(yyjson_doc_get_root(d.get()), s);
        return s;
    }
    static std::string take(char* p, std::size_t len) {
        if (!p) throw std::runtime_error("write failed");
        std::string out(p, len);
        std::free(p);
        return out;
    }
    static std::string write(const Doc& d) {
        std::size_t len = 0;
        return take(yyjson_write(d.get(), 0, &len), len);
    }
    static Built build(const Doc& d) {
        Built b(yyjson_mut_doc_new(nullptr));
        yyjson_mut_doc_set_root(b.get(), copy(b.get(), yyjson_doc_get_root(d.get())));
        return b;
    }
    static std::string write_built(const Built& b) {
        std::size_t len = 0;
        return take(yyjson_mut_write(b.get(), 0, &len), len);
    }
    static std::uint64_t touch(const Doc& d) { return yyjson_get_len(yyjson_doc_get_root(d.get())); }
    static std::uint64_t touch_built(const Built& b) { return yyjson_mut_get_len(yyjson_mut_doc_get_root(b.get())); }
    static std::uint64_t lookup(const Doc& d, const LookupPath& p) {
        std::uint64_t sum = 0;
        yyjson_val* arr = yyjson_obj_getn(yyjson_doc_get_root(d.get()), p.array_key.data(), p.array_key.size());
        yyjson_val* e;
        yyjson_arr_iter it = yyjson_arr_iter_with(arr);
        while ((e = yyjson_arr_iter_next(&it))) {
            yyjson_val* o = yyjson_obj_getn(e, p.object_key.data(), p.object_key.size());
            sum += yyjson_get_len(yyjson_obj_getn(o, p.string_key.data(), p.string_key.size()));
            sum += as_u64(yyjson_obj_getn(e, p.int_key.data(), p.int_key.size()));
            yyjson_val* o2 = yyjson_obj_getn(e, p.object_key2.data(), p.object_key2.size());
            sum += yyjson_arr_size(yyjson_obj_getn(o2, p.array_key2.data(), p.array_key2.size()));
        }
        return sum;
    }
};

#endif

volatile std::uint64_t g_sink = 0;

// Median time of one call of f, in nanoseconds: two warm-up calls, then a batch size that takes at least
// `batch_ns`, then `batches` batches.
template <class F>
double measure(F&& f, double batch_ns, int batches) {
    using clock = std::chrono::steady_clock;
    g_sink = g_sink + f();
    g_sink = g_sink + f();
    std::size_t n = 1;
    for (;;) {
        const auto t0 = clock::now();
        for (std::size_t i = 0; i < n; ++i) g_sink = g_sink + f();
        const double dt = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
        if (dt >= batch_ns) break;
        n *= 2;
    }
    std::vector<double> per_call;
    for (int b = 0; b < batches; ++b) {
        const auto t0 = clock::now();
        for (std::size_t i = 0; i < n; ++i) g_sink = g_sink + f();
        per_call.push_back(std::chrono::duration<double, std::nano>(clock::now() - t0).count() /
                           static_cast<double>(n));
    }
    std::sort(per_call.begin(), per_call.end());
    return per_call[per_call.size() / 2];
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

struct Input {
    std::string name;
    std::string text;
    std::optional<LookupPath> lookup;
};

void usage() {
    std::fprintf(stderr,
                 "usage: bench --file NAME PATH [--lookup ARRAY OBJECT STRING INT OBJECT2 ARRAY2] ... "
                 "[--verify] [--batch-ms MS] [--batches N]\n");
}

int run(int argc, char** argv) {
    std::vector<Input> inputs;
    bool verify = false;
    double batch_ms = 20;
    int batches = 7;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--file" && i + 2 < argc) {
            inputs.push_back({argv[i + 1], read_file(argv[i + 2]), std::nullopt});
            i += 2;
        } else if (a == "--lookup" && i + 6 < argc && !inputs.empty()) {
            inputs.back().lookup = LookupPath{argv[i + 1], argv[i + 2], argv[i + 3],
                                              argv[i + 4], argv[i + 5], argv[i + 6]};
            i += 6;
        } else if (a == "--verify") {
            verify = true;
        } else if (a == "--batch-ms" && i + 1 < argc) {
            batch_ms = std::stod(argv[++i]);
        } else if (a == "--batches" && i + 1 < argc) {
            batches = std::stoi(argv[++i]);
        } else {
            usage();
            return 2;
        }
    }
    if (inputs.empty()) {
        usage();
        return 2;
    }

    for (const auto& in : inputs) {
        const auto doc = Lib::parse(in.text);
        if (verify) {
            // The stats of the file, of its serialization parsed again, and of the serialization of the built copy
            // parsed again must agree; run.py compares the first with the other libraries.
            const Stats s = Lib::stats(doc);
            const Stats s_write = Lib::stats(Lib::parse(Lib::write(doc)));
            const Stats s_build = Lib::stats(Lib::parse(Lib::write_built(Lib::build(doc))));
            std::printf("{\"lib\":\"%s\",\"file\":\"%s\",\"stats\":%s,\"write_round_trip\":%s,"
                        "\"build_round_trip\":%s",
                        BENCH_LIB_NAME, json_escape(in.name).c_str(), s.to_json().c_str(),
                        s_write == s ? "true" : "false", s_build == s ? "true" : "false");
            if (in.lookup) std::printf(",\"lookup\":%llu", (unsigned long long)Lib::lookup(doc, *in.lookup));
            std::printf("}\n");
            continue;
        }
        const double batch_ns = batch_ms * 1e6;
        auto report = [&](const char* workload, double ns) {
            std::printf("{\"lib\":\"%s\",\"file\":\"%s\",\"workload\":\"%s\",\"ns\":%.1f,\"bytes\":%zu}\n",
                        BENCH_LIB_NAME, json_escape(in.name).c_str(), workload, ns, in.text.size());
            std::fflush(stdout);
        };
        report("parse", measure([&] { return Lib::touch(Lib::parse(in.text)); }, batch_ns, batches));
        report("read", measure([&] { return Lib::stats(Lib::parse(in.text)).fold(); }, batch_ns, batches));
        report("write", measure([&] { return static_cast<std::uint64_t>(Lib::write(doc).size()); }, batch_ns,
                                batches));
        report("build", measure([&] { return Lib::touch_built(Lib::build(doc)); }, batch_ns, batches));
        if (in.lookup) {
            report("lookup", measure([&] { return Lib::lookup(doc, *in.lookup); }, batch_ns, batches));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench (%s): %s\n", BENCH_LIB_NAME, e.what());
        return 1;
    }
}
