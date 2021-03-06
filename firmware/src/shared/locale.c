#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define BAD_LEFT      1
#define MISSING_QUOTE 2
#define PREMATURE_EOL 3

#define DEFAULT_LOCALE "en"

static void usage(FILE *f, const char *prog, int exit_code)
{
    fprintf(f ? f : stderr,
	    "usage: %s [locale [infile outfile]]\n"
	    "  locale  -- locale name (default = %s)\n"
	    "  infile  -- input file name (default = menu_<locale>.txt)\n"
	    "  outfile -- output file name (default = menu_<locale>.h)\n",
	    prog ? prog : "locale", DEFAULT_LOCALE);
    exit(exit_code);
}

static int process(const char *line, const char *locale, FILE *f)
{
    char c, name[1024], *ptr, text[1024];
    int done, literal, name_len, text_len;

    if (!locale)
	locale = "c";

    if (!line || !line[0] || !f)
	return(0);

    // Ignore leading white space
    while (isspace(*line))
	++line;

    // Ignore # comments
    if (line[0] == '#')
	return(0);

    // Pass thru '//' comments and blank lines
    if ((line[0] == '/' && line[1] == '/') ||
	(line[0] == '\n'))
    {
	fputs(line, f);
	return(0);
    }
    else if (line[0] == '\0')
    {
	fputc('\n', f);
	return(0);
    }

    literal = 0;

    // Handle the name: it should only have characters in
    // 'a'..'z', 'A'..'Z', '0'..'9' and '_'
    ptr = name;
    name_len = 0;
    while ((c = *line))
    {
	if ( ((c >= 'a') && (c <= 'z')) ||
	     ((c >= 'A') && (c <= 'Z')) ||
	     ((c >= '0') && (c <= '9')) ||
	     (c == '_') )
	{
	    *ptr++ = c;
	    name_len++;
	}
	else if (isspace(c) || (c == '='))
	{
	    // NUL terminate even though we'll use the counted length
	    *ptr = '\0';
	    break;
	}
	else
	    // Syntax error
	    return(BAD_LEFT);

	++line;
    }

    // Premature end of line?
    if (c == '\0')
	return(PREMATURE_EOL);

    // Skip over any whitespace
    while (isspace(*line))
	++line;

    // Must have a '"'
    if (*line++ != '"')
	return(MISSING_QUOTE);

    // We allow \ as an escape character.  The character
    // sets for which this may cause a problem aren't
    // available in the LCD module...

    ptr      = text;
    text_len = 0;
    literal  = 0;
    done     = 0;

    while ((!done) && (c = *line))
    {
	if (literal)
	{
	    if (c == 'n')
		*ptr++ = '\n';
	    else
		*ptr++ = c;
	    text_len++;
	    literal = 0;
	}
	else
	{
	    switch(c)
	    {
	    case '\\' : literal = 1; break;
	    case '"'  : done = 1; break;
	    default   : *ptr++ = c; text_len++; break;
	    }
	}

	++line;
    }

    fprintf(f, "const static PROGMEM prog_uchar %.*s_%s[] = \"%.*s\";\n",
	    name_len, name, locale, text_len, text);

    return(0);
}

int main(int argc, const char *argv[])
{
    char buf[1024], inbuf[1024], outbuf[1024];
    FILE *infile, *outfile;
    const char *infname, *locale, *outfname;
    size_t lineno;
    time_t t;

    if (argc <= 2)
    {
	locale   = (argc == 2) ? argv[1] : DEFAULT_LOCALE;
	infname  = inbuf;
	outfname = outbuf;
	snprintf(inbuf,  sizeof(inbuf),  "menu_%s.txt", locale);
	snprintf(outbuf, sizeof(outbuf), "menu_%s.h",   locale);
    }
    else if (argc == 4)
    {
	locale   = argv[1];
	infname  = argv[2];
	outfname = argv[3];
    }
    else
	usage(stderr, argv[0], 1);

    infile = fopen(infname, "r");
    if (!infile)
    {
	fprintf(stderr, "Unable to open the input file \"%s\"; %s (%d)\n",
		infname, strerror(errno), errno);
	return(1);
    }

    outfile = fopen(outfname, "w");
    if (!outfile)
    {
	fclose(infile);
	fprintf(stderr, "Unable to open the output file \"%s\"; %s (%d)\n",
		outfname, strerror(errno), errno);
	return(1);
    }

    t = time(0);
    fprintf(outfile,
	    "// DO NOT EDIT THIS FILE\n"
	    "// This file was automatically generated from %s by %s\n"
	    "// %s"
	    "\n"
	    "#ifndef MENU_L10N_H_\n"
	    "#define MENU_L10N_H_\n"
	    "\n"
	    "#ifdef LOCALIZE\n"
	    "#undef LOCALIZE\n"
	    "#endif\n"
	    "\n"
	    "#define LOCALIZE(s) s##_%s\n"
	    "\n",
	    infname, argv[0], ctime(&t), locale);

    lineno = 0;
    while(fgets(buf, sizeof(buf), infile))
    {
	int r;

	++lineno;
	if ((r = process(buf, locale, outfile)))
	{
	    fprintf(stderr,
		    "processing terminated; error on line %lu of \"%s\"\n",
		    lineno, infname);

	    switch (r)
	    {
	    default :
		break;

	    case BAD_LEFT :
		fprintf(stderr, "invalid character encountered in name\n");
		break;

	    case MISSING_QUOTE :
		fprintf(stderr,
			"text string did not begin with a double quote, \"\n");
		break;
	
	    case PREMATURE_EOL :
		fprintf(stderr, "premature end-of-line encountered\n");
		break;
	    }

	    fclose(infile);
	    fclose(outfile);
	    unlink(outfname);
	    return(1);
	}
    }

    fprintf(outfile, "\n#endif // MENU_L10N_H_\n");

    fclose(infile);
    fclose(outfile);
    return(0);
}
