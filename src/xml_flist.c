#include <config.h>

//#define _DEBUG

#if defined(HAVE_LIBXML2)

#include <libxml/xmlIO.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlsave.h>

#include "bzip2/bzlib.h"

#include <inttypes.h>		/* ? */
#include "iconvme.h"
#include "common/comparison.h"
#include "common/intutil.h"
#include "common/strbuf.h"
#include "microdc.h"

xmlNodePtr insert_node(xmlNodePtr xml_node, DCFileList* node);
xmlDocPtr  generate_xml_filelist(DCFileList* root);


//#define _TRACE
#if defined(_TRACE)
#include <stdio.h>
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

#if defined(_DEBUG)
FILE* fdebug = 0;
#endif

#define XML_CALL(var, call) \
    var = call; \
    if (var == 0) goto cleanup;

typedef
struct __plain_xml_context
{
    int fd;
} PLAIN_XML_CTXT;

typedef
struct __bzip2_xml_context
{
    BZFILE* file;
} BZIP2_XML_CTXT;


char* xml_quote_string(const unsigned char* str)
{
    int i = 0;
    int len = strlen(str);
    StrBuf *r = strbuf_new();

    for (i = 0; i < len; i++) {
        switch (str[i]) {
        case '&':
            strbuf_append(r, "&amp;");
            break;
        case '<':
            strbuf_append(r, "&lt;");
            break;
        case '>':
            strbuf_append(r, "&gt;");
            break;
        case '\"':
            strbuf_append(r, "&quot;");
            break;
        case '\'':
            strbuf_append(r, "&apos;");
            break;
        default:
	        strbuf_append_char(r, str[i]);
            break;
        }
    }

    return strbuf_free_to_string(r);
}

int xmlTextWriterStartDocumentNew(xmlTextWriterPtr writer, const char *version, const char *encoding, const char *standalone)
{
    int count;
    int sum;

    sum = 0;
    if (version != 0)
        count = xmlTextWriterWriteFormatRaw(writer, "<?xml version=\"%s\"", version);
    else
        count = xmlTextWriterWriteRaw(writer, "<?xml version=\"1.0\"");
    sum += count;
    if (encoding != 0) {
        count = xmlTextWriterWriteFormatRaw(writer, " encoding=\"%s\"", encoding);
        sum += count;
    }
    if (standalone != 0) {
        count = xmlTextWriterWriteFormatRaw(writer, " standalone=\"%s\"", standalone);
        sum += count;
    }
    count = xmlTextWriterWriteRaw(writer, "?>\n");
    sum += count;

    return sum;
}

int xmlTextWriterWriteAttributeFilename(xmlTextWriterPtr writer, const xmlChar * name, const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartAttribute(writer, name);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterWriteRaw(writer, content);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterEndAttribute(writer);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

int write_plain_xml(void* ctxt, const char* buffer, int len)
{
    PLAIN_XML_CTXT* pctxt = (PLAIN_XML_CTXT*)ctxt;
    return write(pctxt->fd, buffer, len);
}

int write_bzip2_xml(void* ctxt, const char* buffer, int len)
{
    BZIP2_XML_CTXT* pctxt = (BZIP2_XML_CTXT*)ctxt;
    return BZ2_bzwrite(pctxt->file, (char*)buffer, len);
}

int read_plain_xml(void* ctxt, char* buffer, int len)
{
    PLAIN_XML_CTXT* pctxt = (PLAIN_XML_CTXT*)ctxt;
    return read(pctxt->fd, buffer, len);
}

int read_bzip2_xml(void* ctxt, char* buffer, int len)
{
    BZIP2_XML_CTXT* pctxt = (BZIP2_XML_CTXT*)ctxt;
    return BZ2_bzread(pctxt->file, (char*)buffer, len);
}

int write_node(xmlTextWriterPtr writer, DCFileList* node)
{
    size_t written = 0;
    if (node != NULL) {
        switch (node->type) {
            case DC_TYPE_REG:
                written += xmlTextWriterStartElement(writer, "File");
                break;
            case DC_TYPE_DIR:
                written += xmlTextWriterStartElement(writer, "Directory");
                break;
            default:
                return 0;
        }

        char* q_name = xml_quote_string(node->name);
        char* utf8_name = fs_to_utf8_string(q_name);
        free(q_name);

        written += xmlTextWriterWriteAttributeFilename(writer, "Name", utf8_name);
        free(utf8_name);

        switch (node->type) {
            case DC_TYPE_REG:
                {
                    char value[64];
                    sprintf(value, "%" PRIu64, node->size);
                    written += xmlTextWriterWriteAttribute(writer, "Size", value);
                    if (node->reg.has_tth) {
                        memset(value, 0, 64);
                        memcpy(value, node->reg.tth, sizeof(node->reg.tth));
                        written += xmlTextWriterWriteAttribute(writer, "TTH", value);
                    }
                }
                break;
            case DC_TYPE_DIR:
                {
                    HMapIterator it;
	                hmap_iterator(node->dir.children, &it);
	                while (it.has_next(&it)) {
	                    DCFileList *subnode = it.next(&it);
                        write_node(writer, subnode);
	                }
                }
                break;
            default:
                return 0;
        }
        written += xmlTextWriterFullEndElement(writer);
    }
    return written;
}

int write_xml_filelist_document(xmlOutputBufferPtr output, DCFileList* root)
{
    size_t written = 0;
    xmlTextWriterPtr writer = xmlNewTextWriter(output);
    xmlTextWriterSetIndent(writer, 0);

    written += xmlTextWriterStartDocumentNew(writer, "1.0", "utf-8", NULL);

    written += xmlTextWriterStartElement(writer, "FileListing");
    written += xmlTextWriterWriteAttribute(writer, "Version", "1");
    written += xmlTextWriterWriteAttribute(writer, "CID", "ABBACDDCEFFE23324554GHHG7667XYYX2RR2XYZ");
    written += xmlTextWriterWriteAttribute(writer, "Generator", my_tag);
    written += xmlTextWriterWriteAttribute(writer, "Base", "/");

    if (root != NULL) {
        HMapIterator it;

	    hmap_iterator(root->dir.children, &it);
	    while (it.has_next(&it)) {
	        DCFileList *node = it.next(&it);
            written += write_node(writer, node);
	    }
    }
    written += xmlTextWriterEndElement(writer);
    written += xmlTextWriterEndDocument(writer);

    xmlFreeTextWriter(writer);
    return written;
}

int write_xml_filelist(int fd, DCFileList* root)
{
    int result = -1;

    PLAIN_XML_CTXT      plain_ctxt;
    plain_ctxt.fd = fd;

    xmlOutputBufferPtr plain_xml = xmlOutputBufferCreateIO(write_plain_xml, NULL, &plain_ctxt, NULL);
    result = write_xml_filelist_document(plain_xml, root);
    // we don't need to free plain_xml because it is deallocated by xmlFreeTextWriter

    return result;
}

int write_bzxml_filelist(int fd, DCFileList* root)
{
    int result = -1;

    BZIP2_XML_CTXT      bzip2_ctxt;
    xmlOutputBufferPtr  bzip2_xml = NULL;

    bzip2_ctxt.file = BZ2_bzdopen(fd, "w");
    if (bzip2_ctxt.file != 0) {
        bzip2_xml = xmlOutputBufferCreateIO(write_bzip2_xml, NULL, &bzip2_ctxt, NULL);
        result = write_xml_filelist_document(bzip2_xml, root);
        BZ2_bzclose(bzip2_ctxt.file);
        // we don't need to free bzip2_xml because it is deallocated by xmlFreeTextWriter
    }
    return result;
}

xmlDocPtr  generate_xml_filelist(DCFileList* root)
{
    xmlDocPtr xml_flist = 0;
    xmlNodePtr xml_root = 0;
    xmlAttrPtr attr = 0;

    XML_CALL(xml_flist, xmlNewDoc(BAD_CAST("1.0")));

    XML_CALL(xml_root, xmlNewNode(NULL, BAD_CAST("FileListing")));

    xmlDocSetRootElement(xml_flist, xml_root);

    XML_CALL(attr, xmlNewProp(xml_root, BAD_CAST("Version"), BAD_CAST("1")));
    XML_CALL(attr, xmlNewProp(xml_root, BAD_CAST("CID"), BAD_CAST("ABBACDDCEFFE23324554GHHG7667XYYX2RR2XYZ")));
    XML_CALL(attr, xmlNewProp(xml_root, BAD_CAST("Generator"), my_tag));
    XML_CALL(attr, xmlNewProp(xml_root, BAD_CAST("Base"), BAD_CAST("/")));

    HMapIterator it;

	hmap_iterator(root->dir.children, &it);
	while (it.has_next(&it)) {
	    DCFileList *subnode = it.next(&it);
        insert_node(xml_root, subnode);
	}

cleanup:

    return xml_flist;
}

xmlNodePtr insert_node(xmlNodePtr xml_parent, DCFileList* node)
{
    xmlNodePtr xml_node = 0;
    xmlAttrPtr attr = 0;
    HMapIterator it;

    switch (node->type) {
    case DC_TYPE_REG:
        XML_CALL(xml_node, xmlNewChild(xml_parent, NULL, BAD_CAST("File"), BAD_CAST("")));
        break;
    case DC_TYPE_DIR:
        XML_CALL(xml_node, xmlNewChild(xml_parent, NULL, BAD_CAST("Directory"), BAD_CAST("")));
	    hmap_iterator(node->dir.children, &it);
	    while (it.has_next(&it)) {
	        DCFileList *subnode = it.next(&it);
            insert_node(xml_node, subnode);
	    }
        break;
    default:
        break;
    }

    if (node != NULL) {
        char* utf8_name = fs_to_utf8_string(node->name);
        
        XML_CALL(attr, xmlNewProp(xml_node, BAD_CAST("Name"), utf8_name));
        free(utf8_name);

        if (node->type == DC_TYPE_REG) {
            char value[64];
            sprintf(value, "%" PRIu64, node->size);
            XML_CALL(attr, xmlNewProp(xml_node, BAD_CAST("Size"), value));
            if (node->reg.has_tth) {
                memset(value, 0, 64);
                memcpy(value, node->reg.tth, sizeof(node->reg.tth));
                XML_CALL(attr, xmlNewProp(xml_node, BAD_CAST("TTH"), value));
            }
        }
    }

    return xml_node;

cleanup:
    return NULL;
}

typedef
struct _parser_state_ctxt
{
    DCFileList*  root;
    DCFileList*  current;
    int unknown_level;
} parser_state_ctxt;

void start_document_callback(void* ctxt)
{
    //TRACE(("%s:%d: start document\n", __FUNCTION__, __LINE__));
}

void end_document_callback(void* ctxt)
{
    //TRACE(("%s:%d: end document\n", __FUNCTION__, __LINE__));
}

void start_element_callback(void* ctxt, const xmlChar* name, const xmlChar** attrs)
{
    parser_state_ctxt* pctxt = (parser_state_ctxt*)ctxt;

    if (pctxt != NULL) {
        char*  filename = NULL;

        //TRACE(("%s:%d: start element <%s>\n", __FUNCTION__, __LINE__, name));

        if (pctxt->unknown_level == 0) {
            char** a = NULL;
            for (a = (const char**)attrs; *a != NULL; a++) {
                char* aname  = *a++;
                char* avalue = *a;
                if (aname != NULL && avalue != NULL) {
                    if (strcasecmp(aname, "Name") == 0) {
                        filename = utf8_to_main_string(avalue);
                    }
                }
            }
        }

        if (pctxt->unknown_level == 0 && strcasecmp(name, "FileListing") == 0) {
            if (pctxt->root != NULL)
                filelist_free(pctxt->root);
            pctxt->root = pctxt->current = new_file_node("", DC_TYPE_DIR, NULL);
        } else if (pctxt->unknown_level == 0 && strcasecmp(name, "Directory") == 0) {
            if (filename == NULL) {
                TRACE(("%s:%d: malformed XML document - unknown directory name\n", __FUNCTION__, __LINE__));
            } else {
                DCFileList* node = new_file_node(filename, DC_TYPE_DIR, pctxt->current);
                pctxt->current = node;
            }
        } else if (pctxt->unknown_level == 0 && strcasecmp(name, "File") == 0) {
            if (filename == NULL) {
                TRACE(("%s:%d: malformed XML document - unknown file name\n", __FUNCTION__, __LINE__));
            } else {
                DCFileList* node = new_file_node(filename, DC_TYPE_REG, pctxt->current);
                pctxt->current = node;
            }
        } else {
            pctxt->unknown_level += 1;
        }

        if (filename != NULL) {
            free(filename);
            filename = NULL;
        }

        if (pctxt->unknown_level == 0 && pctxt->current->type == DC_TYPE_REG) {
            char** a = NULL;
            for (a = (const char**)attrs; *a != NULL; a++) {
                char* aname  = *a++;
                char* avalue = *a;
                if (aname != NULL && avalue != NULL) {
                    if (strcasecmp(aname, "Size") == 0) {
                        uint64_t size;
                        if (!parse_uint64(avalue, &size)) {
                            pctxt->current->size = 0;
                        } else {
                            pctxt->current->size = size;
                            pctxt->current->parent->size += size;
                        }
                    } else if (strcasecmp(aname, "TTH") == 0) {
                        pctxt->current->reg.has_tth = 1;
                        memcpy(pctxt->current->reg.tth, avalue, sizeof(pctxt->current->reg.tth));
                    }
                }
            }
        }
    }
}

void end_element_callback(void* ctxt, const xmlChar* name)
{
    parser_state_ctxt* pctxt = (parser_state_ctxt*)ctxt;
    if (pctxt != NULL) {
        if (pctxt->unknown_level > 0) {
            pctxt->unknown_level -= 1;
        } else {
            if (pctxt->current != NULL) {
                pctxt->current = pctxt->current->parent;
            } else {
                TRACE(("%s:%d: malformed XML document - too many <%s> tags\n", __FUNCTION__, __LINE__, name));
            }
        }
        //TRACE(("%s:%d: end element <%s>\n", __FUNCTION__, __LINE__, name));
    }
}

DCFileList* filelist_xml_open(const char* filename)
{
    DCFileList* root = NULL;
    PLAIN_XML_CTXT io_ctxt;
    io_ctxt.fd = open(filename, O_RDONLY);
    if (io_ctxt.fd >= 0) {
        xmlSAXHandler sax;
        parser_state_ctxt state_ctxt;
        memset(&sax, 0, sizeof(sax));
        memset(&state_ctxt, 0, sizeof(state_ctxt));

        sax.startDocument = start_document_callback;
        sax.endDocument = end_document_callback;
        sax.startElement = start_element_callback;
        sax.endElement = end_element_callback;

        xmlParserCtxtPtr ctxt = xmlCreateIOParserCtxt(&sax, &state_ctxt, read_plain_xml, NULL, &io_ctxt, XML_CHAR_ENCODING_UTF8);
        xmlParseDocument(ctxt);
        xmlFreeParserCtxt(ctxt);

        root = state_ctxt.root;

        close(io_ctxt.fd);
    }

    return root;
}

DCFileList* filelist_bzxml_open(const char* filename)
{
    DCFileList* root = NULL;
    BZIP2_XML_CTXT io_ctxt;
    io_ctxt.file = BZ2_bzopen(filename, "r");
    if (io_ctxt.file != NULL) {
        xmlSAXHandler sax;
        parser_state_ctxt state_ctxt;
        memset(&sax, 0, sizeof(sax));
        memset(&state_ctxt, 0, sizeof(state_ctxt));

        sax.startDocument   = start_document_callback;
        sax.endDocument     = end_document_callback;
        sax.startElement    = start_element_callback;
        sax.endElement      = end_element_callback;

        xmlParserCtxtPtr ctxt = xmlCreateIOParserCtxt(&sax, &state_ctxt, read_bzip2_xml, NULL, &io_ctxt, XML_CHAR_ENCODING_UTF8);
        xmlParseDocument(ctxt);
        xmlFreeParserCtxt(ctxt);

        root = state_ctxt.root;

        BZ2_bzclose(io_ctxt.file);
    }

    return root;
}

#endif // defined(HAVE_LIBXML2)
