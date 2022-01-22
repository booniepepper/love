/**
* Copyright (c) 2006-2022 LOVE Development Team
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
*
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
*
* 1. The origin of this software must not be misrepresented; you must not
*    claim that you wrote the original software. If you use this software
*    in a product, an acknowledgment in the product documentation would be
*    appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
*    misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
**/
#include "common/config.h"
#include "Font.h"
#include "font/GlyphData.h"

#include "libraries/utf8/utf8.h"

#include "common/math.h"
#include "common/Matrix.h"
#include "Graphics.h"

#include <math.h>
#include <sstream>
#include <algorithm> // for max
#include <limits>

namespace love
{
namespace graphics
{

static inline uint16 normToUint16(double n)
{
	return (uint16) (n * LOVE_UINT16_MAX);
}

love::Type Font::type("Font", &Object::type);
int Font::fontCount = 0;

const CommonFormat Font::vertexFormat = CommonFormat::XYf_STus_RGBAub;

Font::Font(love::font::Rasterizer *r, const SamplerState &s)
	: rasterizers({r})
	, height(r->getHeight())
	, lineHeight(1)
	, textureWidth(128)
	, textureHeight(128)
	, samplerState()
	, dpiScale(r->getDPIScale())
	, useSpacesAsTab(false)
	, textureCacheID(0)
{
	samplerState.minFilter = s.minFilter;
	samplerState.magFilter = s.magFilter;
	samplerState.maxAnisotropy = s.maxAnisotropy;

	// Try to find the best texture size match for the font size. default to the
	// largest texture size if no rough match is found.
	while (true)
	{
		if ((height * 0.8) * height * 30 <= textureWidth * textureHeight)
			break;

		TextureSize nextsize = getNextTextureSize();

		if (nextsize.width <= textureWidth && nextsize.height <= textureHeight)
			break;

		textureWidth = nextsize.width;
		textureHeight = nextsize.height;
	}

	love::font::GlyphData *gd = r->getGlyphData(32); // Space character.
	pixelFormat = gd->getFormat();
	gd->release();

	auto gfx = Module::getInstance<Graphics>(Module::M_GRAPHICS);
	if (pixelFormat == PIXELFORMAT_LA8_UNORM && !gfx->isPixelFormatSupported(pixelFormat, PIXELFORMATUSAGEFLAGS_SAMPLE))
		pixelFormat = PIXELFORMAT_RGBA8_UNORM;

	if (!r->hasGlyph(9)) // No tab character in the Rasterizer.
		useSpacesAsTab = true;

	loadVolatile();
	++fontCount;
}

Font::~Font()
{
	--fontCount;
}

Font::TextureSize Font::getNextTextureSize() const
{
	TextureSize size = {textureWidth, textureHeight};

	int maxsize = 2048;
	auto gfx = Module::getInstance<Graphics>(Module::M_GRAPHICS);
	if (gfx != nullptr)
	{
		const auto &caps = gfx->getCapabilities();
		maxsize = (int) caps.limits[Graphics::LIMIT_TEXTURE_SIZE];
	}

	int maxwidth  = std::min(8192, maxsize);
	int maxheight = std::min(4096, maxsize);

	if (size.width * 2 <= maxwidth || size.height * 2 <= maxheight)
	{
		// {128, 128} -> {256, 128} -> {256, 256} -> {512, 256} -> etc.
		if (size.width == size.height)
			size.width *= 2;
		else
			size.height *= 2;
	}

	return size;
}

bool Font::loadVolatile()
{
	textureCacheID++;
	glyphs.clear();
	textures.clear();
	createTexture();
	return true;
}

void Font::createTexture()
{
	auto gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
	gfx->flushBatchedDraws();

	Texture *texture = nullptr;
	TextureSize size = {textureWidth, textureHeight};
	TextureSize nextsize = getNextTextureSize();
	bool recreatetexture = false;

	// If we have an existing texture already, we'll try replacing it with a
	// larger-sized one rather than creating a second one. Having a single
	// texture reduces texture switches and draw calls when rendering.
	if ((nextsize.width > size.width || nextsize.height > size.height) && !textures.empty())
	{
		recreatetexture = true;
		size = nextsize;
		textures.pop_back();
	}

	Texture::Settings settings;
	settings.format = pixelFormat;
	settings.width = size.width;
	settings.height = size.height;
	texture = gfx->newTexture(settings, nullptr);
	texture->setSamplerState(samplerState);

	{
		size_t datasize = getPixelFormatSliceSize(pixelFormat, size.width, size.height);
		size_t pixelcount = size.width * size.height;

		// Initialize the texture with transparent white for truetype fonts
		// (since we keep luminance constant and vary alpha in those glyphs),
		// and transparent black otherwise.
		std::vector<uint8> emptydata(datasize, 0);

		if (rasterizers[0]->getDataType() == font::Rasterizer::DATA_TRUETYPE)
		{
			if (pixelFormat == PIXELFORMAT_LA8_UNORM)
			{
				for (size_t i = 0; i < pixelcount; i++)
					emptydata[i * 2 + 0] = 255;
			}
			else if (pixelFormat == PIXELFORMAT_RGBA8_UNORM)
			{
				for (size_t i = 0; i < pixelcount; i++)
				{
					emptydata[i * 4 + 0] = 255;
					emptydata[i * 4 + 1] = 255;
					emptydata[i * 4 + 2] = 255;
				}
			}
		}

		Rect rect = {0, 0, size.width, size.height};
		texture->replacePixels(emptydata.data(), emptydata.size(), 0, 0, rect, false);
	}

	textures.emplace_back(texture, Acquire::NORETAIN);

	textureWidth  = size.width;
	textureHeight = size.height;

	rowHeight = textureX = textureY = TEXTURE_PADDING;

	// Re-add the old glyphs if we re-created the existing texture object.
	if (recreatetexture)
	{
		textureCacheID++;

		std::vector<uint32> glyphstoadd;

		for (const auto &glyphpair : glyphs)
			glyphstoadd.push_back(glyphpair.first);

		glyphs.clear();
		
		for (uint32 g : glyphstoadd)
			addGlyph(g);
	}
}

void Font::unloadVolatile()
{
	glyphs.clear();
	textures.clear();
}

love::font::GlyphData *Font::getRasterizerGlyphData(uint32 glyph, float &dpiscale)
{
	// Use spaces for the tab 'glyph'.
	if (glyph == 9 && useSpacesAsTab)
	{
		love::font::GlyphData *spacegd = rasterizers[0]->getGlyphData(32);
		PixelFormat fmt = spacegd->getFormat();

		love::font::GlyphMetrics gm = {};
		gm.advance = spacegd->getAdvance() * SPACES_PER_TAB;
		gm.bearingX = spacegd->getBearingX();
		gm.bearingY = spacegd->getBearingY();

		spacegd->release();

		dpiscale = rasterizers[0]->getDPIScale();
		return new love::font::GlyphData(glyph, gm, fmt);
	}

	for (const StrongRef<love::font::Rasterizer> &r : rasterizers)
	{
		if (r->hasGlyph(glyph))
		{
			dpiscale = r->getDPIScale();
			return r->getGlyphData(glyph);
		}
	}

	dpiscale = rasterizers[0]->getDPIScale();
	return rasterizers[0]->getGlyphData(glyph);
}

const Font::Glyph &Font::addGlyph(uint32 glyph)
{
	float glyphdpiscale = getDPIScale();
	StrongRef<love::font::GlyphData> gd(getRasterizerGlyphData(glyph, glyphdpiscale), Acquire::NORETAIN);

	int w = gd->getWidth();
	int h = gd->getHeight();

	if (w + TEXTURE_PADDING * 2 < textureWidth && h + TEXTURE_PADDING * 2 < textureHeight)
	{
		if (textureX + w + TEXTURE_PADDING > textureWidth)
		{
			// Out of space - new row!
			textureX = TEXTURE_PADDING;
			textureY += rowHeight;
			rowHeight = TEXTURE_PADDING;
		}

		if (textureY + h + TEXTURE_PADDING > textureHeight)
		{
			// Totally out of space - new texture!
			createTexture();

			// Makes sure the above code for checking if the glyph can fit at
			// the current position in the texture is run again for this glyph.
			return addGlyph(glyph);
		}
	}

	Glyph g;

	g.texture = 0;
	g.spacing = floorf(gd->getAdvance() / glyphdpiscale + 0.5f);

	memset(g.vertices, 0, sizeof(GlyphVertex) * 4);

	// Don't waste space for empty glyphs.
	if (w > 0 && h > 0)
	{
		Texture *texture = textures.back();
		g.texture = texture;

		Rect rect = {textureX, textureY, gd->getWidth(), gd->getHeight()};

		if (pixelFormat != gd->getFormat())
		{
			if (!(pixelFormat == PIXELFORMAT_RGBA8_UNORM && gd->getFormat() == PIXELFORMAT_LA8_UNORM))
				throw love::Exception("Cannot upload font glyphs to texture atlas: unexpected format conversion.");

			const uint8 *src = (const uint8 *) gd->getData();

			size_t dstsize = getPixelFormatSliceSize(pixelFormat, w, h);
			std::vector<uint8> dst(dstsize, 0);
			uint8 *dstdata = dst.data();

			for (int pixel = 0; pixel < w * h; pixel++)
			{
				dstdata[pixel * 4 + 0] = src[pixel * 2 + 0];
				dstdata[pixel * 4 + 1] = src[pixel * 2 + 0];
				dstdata[pixel * 4 + 2] = src[pixel * 2 + 0];
				dstdata[pixel * 4 + 3] = src[pixel * 2 + 1];
			}

			texture->replacePixels(dstdata, dstsize, 0, 0, rect, false);
		}
		else
		{
			texture->replacePixels(gd->getData(), gd->getSize(), 0, 0, rect, false);
		}

		double tX     = (double) textureX,     tY      = (double) textureY;
		double tWidth = (double) textureWidth, tHeight = (double) textureHeight;

		Color32 c(255, 255, 255, 255);

		// Extrude the quad borders by 1 pixel. We have an extra pixel of
		// transparent padding in the texture atlas, so the quad extrusion will
		// add some antialiasing at the edges of the quad.
		int o = 1;

		// 0---2
		// | / |
		// 1---3
		const GlyphVertex verts[4] =
		{
			{float(-o),      float(-o),                normToUint16((tX-o)/tWidth),   normToUint16((tY-o)/tHeight),   c},
			{float(-o),      (h+o)/glyphdpiscale,      normToUint16((tX-o)/tWidth),   normToUint16((tY+h+o)/tHeight), c},
			{(w+o)/glyphdpiscale, float(-o),           normToUint16((tX+w+o)/tWidth), normToUint16((tY-o)/tHeight),   c},
			{(w+o)/glyphdpiscale, (h+o)/glyphdpiscale, normToUint16((tX+w+o)/tWidth), normToUint16((tY+h+o)/tHeight), c}
		};

		// Copy vertex data to the glyph and set proper bearing.
		for (int i = 0; i < 4; i++)
		{
			g.vertices[i] = verts[i];
			g.vertices[i].x += gd->getBearingX() / glyphdpiscale;
			g.vertices[i].y -= gd->getBearingY() / glyphdpiscale;
		}

		textureX += w + TEXTURE_PADDING;
		rowHeight = std::max(rowHeight, h + TEXTURE_PADDING);
	}

	glyphs[glyph] = g;
	return glyphs[glyph];
}

const Font::Glyph &Font::findGlyph(uint32 glyph)
{
	const auto it = glyphs.find(glyph);

	if (it != glyphs.end())
		return it->second;

	return addGlyph(glyph);
}

float Font::getKerning(uint32 leftglyph, uint32 rightglyph)
{
	uint64 packedglyphs = ((uint64) leftglyph << 32) | (uint64) rightglyph;

	const auto it = kerning.find(packedglyphs);
	if (it != kerning.end())
		return it->second;

	float k = floorf(rasterizers[0]->getKerning(leftglyph, rightglyph) / dpiScale + 0.5f);

	for (const auto &r : rasterizers)
	{
		if (r->hasGlyph(leftglyph) && r->hasGlyph(rightglyph))
		{
			k = floorf(r->getKerning(leftglyph, rightglyph) / r->getDPIScale() + 0.5f);
			break;
		}
	}

	kerning[packedglyphs] = k;
	return k;
}

float Font::getKerning(const std::string &leftchar, const std::string &rightchar)
{
	uint32 left = 0;
	uint32 right = 0;

	try
	{
		left = utf8::peek_next(leftchar.begin(), leftchar.end());
		right = utf8::peek_next(rightchar.begin(), rightchar.end());
	}
	catch (utf8::exception &e)
	{
		throw love::Exception("UTF-8 decoding error: %s", e.what());
	}

	return getKerning(left, right);
}

void Font::getCodepointsFromString(const std::string &text, Codepoints &codepoints)
{
	codepoints.reserve(text.size());

	try
	{
		utf8::iterator<std::string::const_iterator> i(text.begin(), text.begin(), text.end());
		utf8::iterator<std::string::const_iterator> end(text.end(), text.begin(), text.end());

		while (i != end)
		{
			uint32 g = *i++;
			codepoints.push_back(g);
		}
	}
	catch (utf8::exception &e)
	{
		throw love::Exception("UTF-8 decoding error: %s", e.what());
	}
}

void Font::getCodepointsFromString(const std::vector<ColoredString> &strs, ColoredCodepoints &codepoints)
{
	if (strs.empty())
		return;

	codepoints.cps.reserve(strs[0].str.size());

	for (const ColoredString &cstr : strs)
	{
		// No need to add the color if the string is empty anyway, and the code
		// further on assumes no two colors share the same starting position.
		if (cstr.str.size() == 0)
			continue;

		IndexedColor c = {cstr.color, (int) codepoints.cps.size()};
		codepoints.colors.push_back(c);

		getCodepointsFromString(cstr.str, codepoints.cps);
	}

	if (codepoints.colors.size() == 1)
	{
		IndexedColor c = codepoints.colors[0];

		if (c.index == 0 && c.color == Colorf(1.0f, 1.0f, 1.0f, 1.0f))
			codepoints.colors.pop_back();
	}
}

float Font::getHeight() const
{
	return (float) floorf(height / dpiScale + 0.5f);
}

std::vector<Font::DrawCommand> Font::generateVertices(const ColoredCodepoints &codepoints, const Colorf &constantcolor, std::vector<GlyphVertex> &vertices, float extra_spacing, Vector2 offset, TextInfo *info)
{
	// Spacing counter and newline handling.
	float dx = offset.x;
	float dy = offset.y;

	float heightoffset = 0.0f;

	if (rasterizers[0]->getDataType() == font::Rasterizer::DATA_TRUETYPE)
		heightoffset = getBaseline();

	int maxwidth = 0;

	// Keeps track of when we need to switch textures in our vertex array.
	std::vector<DrawCommand> commands;

	// Pre-allocate space for the maximum possible number of vertices.
	size_t vertstartsize = vertices.size();
	vertices.reserve(vertstartsize + codepoints.cps.size() * 4);

	uint32 prevglyph = 0;

	Colorf linearconstantcolor = gammaCorrectColor(constantcolor);

	Color32 curcolor = toColor32(constantcolor);
	int curcolori = -1;
	int ncolors = (int) codepoints.colors.size();

	for (int i = 0; i < (int) codepoints.cps.size(); i++)
	{
		uint32 g = codepoints.cps[i];

		if (curcolori + 1 < ncolors && codepoints.colors[curcolori + 1].index == i)
		{
			Colorf c = codepoints.colors[++curcolori].color;

			c.r = std::min(std::max(c.r, 0.0f), 1.0f);
			c.g = std::min(std::max(c.g, 0.0f), 1.0f);
			c.b = std::min(std::max(c.b, 0.0f), 1.0f);
			c.a = std::min(std::max(c.a, 0.0f), 1.0f);

			gammaCorrectColor(c);
			c *= linearconstantcolor;
			unGammaCorrectColor(c);

			curcolor = toColor32(c);
		}

		if (g == '\n')
		{
			if (dx > maxwidth)
				maxwidth = (int) dx;

			// Wrap newline, but do not print it.
			dy += floorf(getHeight() * getLineHeight() + 0.5f);
			dx = offset.x;
			prevglyph = 0;
			continue;
		}

		// Ignore carriage returns
		if (g == '\r')
			continue;

		uint32 cacheid = textureCacheID;

		const Glyph &glyph = findGlyph(g);

		// If findGlyph invalidates the texture cache, re-start the loop.
		if (cacheid != textureCacheID)
		{
			i = -1; // The next iteration will increment this to 0.
			maxwidth = 0;
			dx = offset.x;
			dy = offset.y;
			commands.clear();
			vertices.resize(vertstartsize);
			prevglyph = 0;
			curcolori = -1;
			curcolor = toColor32(constantcolor);
			continue;
		}

		// Add kerning to the current horizontal offset.
		dx += getKerning(prevglyph, g);

		if (glyph.texture != nullptr)
		{
			// Copy the vertices and set their colors and relative positions.
			for (int j = 0; j < 4; j++)
			{
				vertices.push_back(glyph.vertices[j]);
				vertices.back().x += dx;
				vertices.back().y += dy + heightoffset;
				vertices.back().color = curcolor;
			}

			// Check if glyph texture has changed since the last iteration.
			if (commands.empty() || commands.back().texture != glyph.texture)
			{
				// Add a new draw command if the texture has changed.
				DrawCommand cmd;
				cmd.startvertex = (int) vertices.size() - 4;
				cmd.vertexcount = 0;
				cmd.texture = glyph.texture;
				commands.push_back(cmd);
			}

			commands.back().vertexcount += 4;
		}

		// Advance the x position for the next glyph.
		dx += glyph.spacing;

		// Account for extra spacing given to space characters.
		if (g == ' ' && extra_spacing != 0.0f)
			dx = floorf(dx + extra_spacing);

		prevglyph = g;
	}

	const auto drawsort = [](const DrawCommand &a, const DrawCommand &b) -> bool
	{
		// Texture binds are expensive, so we should sort by that first.
		if (a.texture != b.texture)
			return a.texture < b.texture;
		else
			return a.startvertex < b.startvertex;
	};

	std::sort(commands.begin(), commands.end(), drawsort);

	if (dx > maxwidth)
		maxwidth = (int) dx;

	if (info != nullptr)
	{
		info->width = maxwidth - offset.x;
		info->height = (int) dy + (dx > 0.0f ? floorf(getHeight() * getLineHeight() + 0.5f) : 0) - offset.y;
	}

	return commands;
}

std::vector<Font::DrawCommand> Font::generateVerticesFormatted(const ColoredCodepoints &text, const Colorf &constantcolor, float wrap, AlignMode align, std::vector<GlyphVertex> &vertices, TextInfo *info)
{
	wrap = std::max(wrap, 0.0f);

	uint32 cacheid = textureCacheID;

	std::vector<DrawCommand> drawcommands;
	vertices.reserve(text.cps.size() * 4);

	std::vector<int> widths;
	std::vector<ColoredCodepoints> lines;

	getWrap(text, wrap, lines, &widths);

	float y = 0.0f;
	float maxwidth = 0.0f;

	for (int i = 0; i < (int) lines.size(); i++)
	{
		const auto &line = lines[i];

		float width = (float) widths[i];
		love::Vector2 offset(0.0f, floorf(y));
		float extraspacing = 0.0f;

		maxwidth = std::max(width, maxwidth);

		switch (align)
		{
			case ALIGN_RIGHT:
				offset.x = floorf(wrap - width);
				break;
			case ALIGN_CENTER:
				offset.x = floorf((wrap - width) / 2.0f);
				break;
			case ALIGN_JUSTIFY:
			{
				float numspaces = (float) std::count(line.cps.begin(), line.cps.end(), ' ');
				if (width < wrap && numspaces >= 1)
					extraspacing = (wrap - width) / numspaces;
				else
					extraspacing = 0.0f;
				break;
			}
			case ALIGN_LEFT:
			default:
				break;
		}

		std::vector<DrawCommand> newcommands = generateVertices(line, constantcolor, vertices, extraspacing, offset);

		if (!newcommands.empty())
		{
			auto firstcmd = newcommands.begin();

			// If the first draw command in the new list has the same texture
			// as the last one in the existing list we're building and its
			// vertices are in-order, we can combine them (saving a draw call.)
			if (!drawcommands.empty())
			{
				auto prevcmd = drawcommands.back();
				if (prevcmd.texture == firstcmd->texture && (prevcmd.startvertex + prevcmd.vertexcount) == firstcmd->startvertex)
				{
					drawcommands.back().vertexcount += firstcmd->vertexcount;
					++firstcmd;
				}
			}

			// Append the new draw commands to the list we're building.
			drawcommands.insert(drawcommands.end(), firstcmd, newcommands.end());
		}

		y += getHeight() * getLineHeight();
	}

	if (info != nullptr)
	{
		info->width = (int) maxwidth;
		info->height = (int) y;
	}

	if (cacheid != textureCacheID)
	{
		vertices.clear();
		drawcommands = generateVerticesFormatted(text, constantcolor, wrap, align, vertices);
	}

	return drawcommands;
}

void Font::printv(graphics::Graphics *gfx, const Matrix4 &t, const std::vector<DrawCommand> &drawcommands, const std::vector<GlyphVertex> &vertices)
{
	if (vertices.empty() || drawcommands.empty())
		return;

	Matrix4 m(gfx->getTransform(), t);

	for (const DrawCommand &cmd : drawcommands)
	{
		Graphics::BatchedDrawCommand streamcmd;
		streamcmd.formats[0] = vertexFormat;
		streamcmd.indexMode = TRIANGLEINDEX_QUADS;
		streamcmd.vertexCount = cmd.vertexcount;
		streamcmd.texture = cmd.texture;

		Graphics::BatchedVertexData data = gfx->requestBatchedDraw(streamcmd);
		GlyphVertex *vertexdata = (GlyphVertex *) data.stream[0];

		memcpy(vertexdata, &vertices[cmd.startvertex], sizeof(GlyphVertex) * cmd.vertexcount);
		m.transformXY(vertexdata, &vertices[cmd.startvertex], cmd.vertexcount);
	}
}

void Font::print(graphics::Graphics *gfx, const std::vector<ColoredString> &text, const Matrix4 &m, const Colorf &constantcolor)
{
	ColoredCodepoints codepoints;
	getCodepointsFromString(text, codepoints);

	std::vector<GlyphVertex> vertices;
	std::vector<DrawCommand> drawcommands = generateVertices(codepoints, constantcolor, vertices);

	printv(gfx, m, drawcommands, vertices);
}

void Font::printf(graphics::Graphics *gfx, const std::vector<ColoredString> &text, float wrap, AlignMode align, const Matrix4 &m, const Colorf &constantcolor)
{
	ColoredCodepoints codepoints;
	getCodepointsFromString(text, codepoints);

	std::vector<GlyphVertex> vertices;
	std::vector<DrawCommand> drawcommands = generateVerticesFormatted(codepoints, constantcolor, wrap, align, vertices);

	printv(gfx, m, drawcommands, vertices);
}

int Font::getWidth(const std::string &str)
{
	if (str.size() == 0) return 0;

	std::istringstream iss(str);
	std::string line;
	int max_width = 0;

	while (getline(iss, line, '\n'))
	{
		int width = 0;
		uint32 prevglyph = 0;
		try
		{
			utf8::iterator<std::string::const_iterator> i(line.begin(), line.begin(), line.end());
			utf8::iterator<std::string::const_iterator> end(line.end(), line.begin(), line.end());

			while (i != end)
			{
				uint32 c = *i++;

				// Ignore carriage returns
				if (c == '\r')
					continue;

				const Glyph &g = findGlyph(c);
				width += g.spacing + getKerning(prevglyph, c);

				prevglyph = c;
			}
		}
		catch (utf8::exception &e)
		{
			throw love::Exception("UTF-8 decoding error: %s", e.what());
		}

		max_width = std::max(max_width, width);
	}

	return max_width;
}

int Font::getWidth(uint32 glyph)
{
	const Glyph &g = findGlyph(glyph);
	return g.spacing;
}

void Font::getWrap(const ColoredCodepoints &codepoints, float wraplimit, std::vector<ColoredCodepoints> &lines, std::vector<int> *linewidths)
{
	// Per-line info.
	float width = 0.0f;
	float widthbeforelastspace = 0.0f;
	float widthoftrailingspace = 0.0f;
	uint32 prevglyph = 0;

	int lastspaceindex = -1;

	// Keeping the indexed colors "in sync" is a bit tricky, since we split
	// things up and we might skip some glyphs but we don't want to skip any
	// color which starts at those indices.
	Colorf curcolor(1.0f, 1.0f, 1.0f, 1.0f);
	bool addcurcolor = false;
	int curcolori = -1;
	int endcolori = (int) codepoints.colors.size() - 1;

	// A wrapped line of text.
	ColoredCodepoints wline;

	int i = 0;
	while (i < (int) codepoints.cps.size())
	{
		uint32 c = codepoints.cps[i];

		// Determine the current color before doing anything else, to make sure
		// it's still applied to future glyphs even if this one is skipped.
		if (curcolori < endcolori && codepoints.colors[curcolori + 1].index == i)
		{
			curcolor = codepoints.colors[curcolori + 1].color;
			curcolori++;
			addcurcolor = true;
		}

		// Split text at newlines.
		if (c == '\n')
		{
			lines.push_back(wline);

			// Ignore the width of any trailing spaces, for individual lines.
			if (linewidths)
				linewidths->push_back(width - widthoftrailingspace);

			// Make sure the new line keeps any color that was set previously.
			addcurcolor = true;

			width = widthbeforelastspace = widthoftrailingspace = 0.0f;
			prevglyph = 0; // Reset kerning information.
			lastspaceindex = -1;
			wline.cps.clear();
			wline.colors.clear();
			i++;

			continue;
		}

		// Ignore carriage returns
		if (c == '\r')
		{
			i++;
			continue;
		}

		const Glyph &g = findGlyph(c);
		float charwidth = g.spacing + getKerning(prevglyph, c);
		float newwidth = width + charwidth;

		// Wrap the line if it exceeds the wrap limit. Don't wrap yet if we're
		// processing a newline character, though.
		if (c != ' ' && newwidth > wraplimit)
		{
			// If this is the first character in the line and it exceeds the
			// limit, skip it completely.
			if (wline.cps.empty())
				i++;
			else if (lastspaceindex != -1)
			{
				// 'Rewind' to the last seen space, if the line has one.
				// FIXME: This could be more efficient...
				while (!wline.cps.empty() && wline.cps.back() != ' ')
					wline.cps.pop_back();

				while (!wline.colors.empty() && wline.colors.back().index >= (int) wline.cps.size())
					wline.colors.pop_back();

				// Also 'rewind' to the color that the last character is using.
				for (int colori = curcolori; colori >= 0; colori--)
				{
					if (codepoints.colors[colori].index <= lastspaceindex)
					{
						curcolor = codepoints.colors[colori].color;
						curcolori = colori;
						break;
					}
				}

				// Ignore the width of trailing spaces in wrapped lines.
				width = widthbeforelastspace;

				i = lastspaceindex;
				i++; // Start the next line after the space.
			}

			lines.push_back(wline);

			if (linewidths)
				linewidths->push_back(width);

			addcurcolor = true;

			prevglyph = 0;
			width = widthbeforelastspace = widthoftrailingspace = 0.0f;
			wline.cps.clear();
			wline.colors.clear();
			lastspaceindex = -1;

			continue;
		}

		if (prevglyph != ' ' && c == ' ')
			widthbeforelastspace = width;

		width = newwidth;
		prevglyph = c;

		if (addcurcolor)
		{
			wline.colors.push_back({curcolor, (int) wline.cps.size()});
			addcurcolor = false;
		}

		wline.cps.push_back(c);

		// Keep track of the last seen space, so we can "rewind" to it when
		// wrapping.
		if (c == ' ')
		{
			lastspaceindex = i;
			widthoftrailingspace += charwidth;
		}
		else if (c != '\n')
			widthoftrailingspace = 0.0f;

		i++;
	}

	// Push the last line.
	lines.push_back(wline);

	// Ignore the width of any trailing spaces, for individual lines.
	if (linewidths)
		linewidths->push_back(width - widthoftrailingspace);
}

void Font::getWrap(const std::vector<ColoredString> &text, float wraplimit, std::vector<std::string> &lines, std::vector<int> *linewidths)
{
	ColoredCodepoints cps;
	getCodepointsFromString(text, cps);

	std::vector<ColoredCodepoints> codepointlines;
	getWrap(cps, wraplimit, codepointlines, linewidths);

	std::string line;

	for (const ColoredCodepoints &codepoints : codepointlines)
	{
		line.clear();
		line.reserve(codepoints.cps.size());

		for (uint32 codepoint : codepoints.cps)
		{
			char character[5] = {'\0'};
			char *end = utf8::unchecked::append(codepoint, character);
			line.append(character, end - character);
		}

		lines.push_back(line);
	}
}

void Font::setLineHeight(float height)
{
	lineHeight = height;
}

float Font::getLineHeight() const
{
	return lineHeight;
}

void Font::setSamplerState(const SamplerState &s)
{
	samplerState.minFilter = s.minFilter;
	samplerState.magFilter = s.magFilter;
	samplerState.maxAnisotropy = s.maxAnisotropy;

	for (const auto &texture : textures)
		texture->setSamplerState(samplerState);
}

const SamplerState &Font::getSamplerState() const
{
	return samplerState;
}

int Font::getAscent() const
{
	return floorf(rasterizers[0]->getAscent() / dpiScale + 0.5f);
}

int Font::getDescent() const
{
	return floorf(rasterizers[0]->getDescent() / dpiScale + 0.5f);
}

float Font::getBaseline() const
{
	float ascent = getAscent();
	if (ascent != 0.0f)
		return ascent;
	else if (rasterizers[0]->getDataType() == font::Rasterizer::DATA_TRUETYPE)
		return floorf(getHeight() / 1.25f + 0.5f); // 1.25 is magic line height for true type fonts
	else
		return 0.0f;
}

bool Font::hasGlyph(uint32 glyph) const
{
	for (const StrongRef<love::font::Rasterizer> &r : rasterizers)
	{
		if (r->hasGlyph(glyph))
			return true;
	}

	return false;
}

bool Font::hasGlyphs(const std::string &text) const
{
	if (text.size() == 0)
		return false;

	try
	{
		utf8::iterator<std::string::const_iterator> i(text.begin(), text.begin(), text.end());
		utf8::iterator<std::string::const_iterator> end(text.end(), text.begin(), text.end());
		
		while (i != end)
		{
			uint32 codepoint = *i++;
			
			if (!hasGlyph(codepoint))
				return false;
		}
	}
	catch (utf8::exception &e)
	{
		throw love::Exception("UTF-8 decoding error: %s", e.what());
	}

	return true;
}

void Font::setFallbacks(const std::vector<Font *> &fallbacks)
{
	for (const Font *f : fallbacks)
	{
		if (f->rasterizers[0]->getDataType() != this->rasterizers[0]->getDataType())
			throw love::Exception("Font fallbacks must be of the same font type.");
	}

	rasterizers.resize(1);

	// NOTE: this won't invalidate already-rasterized glyphs.
	for (const Font *f : fallbacks)
		rasterizers.push_back(f->rasterizers[0]);
}

float Font::getDPIScale() const
{
	return dpiScale;
}

uint32 Font::getTextureCacheID() const
{
	return textureCacheID;
}

bool Font::getConstant(const char *in, AlignMode &out)
{
	return alignModes.find(in, out);
}

bool Font::getConstant(AlignMode in, const char  *&out)
{
	return alignModes.find(in, out);
}

std::vector<std::string> Font::getConstants(AlignMode)
{
	return alignModes.getNames();
}

StringMap<Font::AlignMode, Font::ALIGN_MAX_ENUM>::Entry Font::alignModeEntries[] =
{
	{ "left", ALIGN_LEFT },
	{ "right", ALIGN_RIGHT },
	{ "center", ALIGN_CENTER },
	{ "justify", ALIGN_JUSTIFY },
};

StringMap<Font::AlignMode, Font::ALIGN_MAX_ENUM> Font::alignModes(Font::alignModeEntries, sizeof(Font::alignModeEntries));

} // graphics
} // love
