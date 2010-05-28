<!-- $Cambridge: exim/exim-doc/doc-docbook/MyStyle-txt-html.xsl,v 1.4 2010/05/28 15:38:18 nm4 Exp $ -->

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version='1.0'>

<!-- This stylesheet driver imports the DocBook XML stylesheet for unchunked
HTML output, and then imports my common stylesheet for HTML output. Then it
adds an instruction to use "(c)" for copyright rather than the Unicode
character. -->

<xsl:import href="/usr/share/xml/docbook/stylesheet/nwalsh/1.71.1/xhtml/docbook.xsl"/>
<xsl:import href="MyStyle-html.xsl"/>

<xsl:template name="dingbat.characters">
  <xsl:param name="dingbat">bullet</xsl:param>
  <xsl:choose>
    <xsl:when test="$dingbat='copyright'">(c)</xsl:when>
    <xsl:otherwise>
      <xsl:text>?</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

</xsl:stylesheet>
