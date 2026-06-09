// Demonstrates the second defect: a libxml2 allocation failure (memory pressure) on a
// perfectly VALID document makes xmlCtxtReadMemory return NULL, which webbed reports as
// "File X contains invalid XML" -- indistinguishable, to the current code, from a
// genuinely malformed file.
//
// It also proves the fix is feasible: xmlCtxtGetLastError()->code reports XML_ERR_NO_MEMORY
// for the allocation-failure case but a structural code (e.g. XML_ERR_GT_REQUIRED /
// XML_ERR_TAG_NOT_FINISHED) for a genuinely malformed document. The constructor can read
// this BEFORE freeing the parser context and surface a distinct resource error.
//
// REQUIRES Linux libxml2: Apple's system libxml2 (macOS 15.4+) makes xmlMemSetup a no-op.
// Run under Docker:
//   docker run --rm -v "$PWD/tmp":/src:ro catthehacker/ubuntu:act-latest bash -c \
//     'g++ -std=c++17 -O2 /src/repro_oom_mislabel.cpp $(xml2-config --cflags --libs) -o /tmp/o && /tmp/o'

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlmemory.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::atomic<long> g_count{0};
static std::atomic<long> g_fail_after{-1}; // -1 = never fail

static bool should_fail() {
	long c = g_count.fetch_add(1);
	long f = g_fail_after.load();
	return f >= 0 && c >= f;
}
static void *my_malloc(size_t n) { return should_fail() ? nullptr : malloc(n); }
static void my_free(void *p) { if (p) free(p); }
static void *my_realloc(void *p, size_t n) { return should_fail() ? nullptr : realloc(p, n); }
static char *my_strdup(const char *s) { return should_fail() ? nullptr : strdup(s); }

// Parse, capturing whether doc is NULL and the last-error code -- mirrors what the
// XMLDocRAII constructor could inspect before xmlFreeParserCtxt.
struct ParseResult {
	bool null_doc;
	int err_code;
	std::string err_name;
};
static ParseResult parse(const std::string &xml) {
	ParseResult r{true, 0, "none"};
	xmlParserCtxtPtr ctx = xmlNewParserCtxt();
	if (!ctx) {
		return r;
	}
	xmlDocPtr doc = xmlCtxtReadMemory(ctx, xml.c_str(), xml.length(), nullptr, nullptr,
	                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	r.null_doc = (doc == nullptr);
	const xmlError *e = xmlCtxtGetLastError(ctx); // <-- read BEFORE freeing the ctxt
	if (e) {
		r.err_code = e->code;
		r.err_name = (e->code == XML_ERR_NO_MEMORY) ? "XML_ERR_NO_MEMORY" : "structural/parse error";
	}
	if (doc) {
		xmlFreeDoc(doc);
	}
	xmlFreeParserCtxt(ctx);
	return r;
}

int main() {
	// xmlMemSetup must run before any libxml2 allocation. It returns 0 on success and -1
	// if the custom allocator could not be installed; without it the repro cannot inject
	// allocation failures, so fail fast rather than print a misleading "inconclusive".
	if (xmlMemSetup(my_free, my_malloc, my_realloc, my_strdup) != 0) {
		std::fprintf(stderr, "xmlMemSetup failed: custom allocator could not be installed on this libxml2 build\n");
		return 2;
	}
	xmlInitParser();

	std::string valid = "<root>";
	for (int i = 0; i < 200; ++i) {
		valid += "<item id=\"" + std::to_string(i) + "\">value " + std::to_string(i) + "</item>";
	}
	valid += "</root>";

	// 1) Baseline: the document is valid with no memory pressure.
	g_fail_after = -1;
	g_count = 0;
	ParseResult base = parse(valid);
	std::printf("baseline (no pressure):       null_doc=%d  -> valid document, parses fine\n", base.null_doc);

	// 2) Sweep allocation-failure points across a valid parse; count how many produce a
	//    NULL doc (which webbed maps to "contains invalid XML").
	long null_cases = 0, first_oom_fail_point = -1;
	for (long f = 1; f <= 200; ++f) {
		g_count = 0;
		g_fail_after = f;
		ParseResult r = parse(valid);
		if (r.null_doc) {
			null_cases++;
			if (first_oom_fail_point < 0 && r.err_code == XML_ERR_NO_MEMORY) {
				first_oom_fail_point = f;
			}
		}
	}
	std::printf("memory pressure on VALID doc: %ld of 200 allocation-failure points yield a NULL doc\n",
	            null_cases);
	std::printf("                              -> webbed currently throws \"contains invalid XML\"\n");

	// 3) Show the failure is distinguishable: OOM case vs a genuinely malformed doc.
	g_count = 0;
	g_fail_after = (first_oom_fail_point > 0 ? first_oom_fail_point : 20);
	ParseResult oom = parse(valid);
	g_fail_after = -1; // no pressure
	g_count = 0;
	ParseResult malformed = parse("<root><unclosed>");

	std::printf("\ndistinguishability (the basis for the fix):\n");
	std::printf("  valid doc + allocation failure: null_doc=%d  last_error=%s (code %d)\n", oom.null_doc,
	            oom.err_name.c_str(), oom.err_code);
	std::printf("  genuinely malformed doc:        null_doc=%d  last_error=%s (code %d)\n", malformed.null_doc,
	            malformed.err_name.c_str(), malformed.err_code);

	bool proven = (null_cases > 0) && (oom.err_code == XML_ERR_NO_MEMORY) &&
	              (malformed.err_code != XML_ERR_NO_MEMORY);
	std::printf("\n%s\n", proven ? "DEMONSTRATED: valid docs are mislabeled \"invalid XML\" under memory pressure, "
	                                "AND XML_ERR_NO_MEMORY distinguishes it from real malformed input."
	                              : "inconclusive on this libxml2 build");
	xmlCleanupParser();
	return proven ? 0 : 1;
}
